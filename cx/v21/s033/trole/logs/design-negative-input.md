# C++ target receive-role 인계 설계 — #1639

## 1. 판정과 범위

**수신 회전이 끝난 뒤 기존 offload executor가 idle worker 하나에 수신 역할을 독점 배정하고, 이전 수신자가 기존 owner drain에 진입하는 제한 실험을 제안한다.** 배정 실패 시 현재 경로를 유지한다. 이는 S08 §4.4의 기존 계약 적응(A)이며, 제품 구현 승인이나 성능 개선 판정이 아니다(S08:302–316; `chand/report.md:65–75,112`; `cadvice/report.md:294–301,325–338`).

다만 **현재 기준에서는 같은 Node/Channel owner의 동기 handler A를 붙잡은 동안 B가 독립 실행되는 조건을 충족한다고 설계만으로 보장할 수 없다.** Mailbox가 prefix 실행 중 다음 claim을 차단하기 때문이다(C/mesh/service_mailbox.cpp:212–217,235–237,507–514; C/stateful/public_host_runtime.cpp:8535–8555). 역할 배치와 다른 기존 계약 결함(B) 후보이며, per-record 재제출로 해결할 수 없다(`p2/report.md:9–15`). §8의 선행 차단 조건을 PM에게 함께 제출해야 한다. 이 문서는 해당 결함을 해결하는 executor 전체 개편을 승인 대상으로 포함하지 않는다.

기준은 `/root/worktree/zlink-v21-s033/trole`, `perf/1639-cpp-target-receive-role`, HEAD `76a4d7850664c2cb32e8d35837dac45d9d2a4057`이다(`logs/repository-state.txt:1–4`). 제품·spec 변경, 빌드, 기능 시험, 새 성능 측정은 수행하지 않았다. 아래 알고리즘·개수·시험은 모두 제안이다.

### 근거 경로

이 문서의 `C/`는 `framework/languages/cpp/framework/src/runtime/`, `CT/`는 `framework/languages/cpp/tests/Zlink.Framework.UnitTests/`이다. `J/`는 `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/`, `N/`은 `framework/languages/node/packages/framework/src/runtime/`이다. `D@6821/`는 **`/root/zlink`에서 `git show 6821c3b8af:<path>`로 읽은** `framework/languages/dotnet/src/Zlink.Framework/Runtime/`이며 현재 HEAD의 줄 번호가 아니다.

`S01`, `S02`, `S04`, `S08`은 `framework/doc/framework/common/spec/server/01-execution/` 아래 각각 `01-submit-and-completion.ko.md`, `02-handler-turn-and-execution-gate.ko.md`, `04-application-job-queue-and-backpressure.ko.md`, `08-messaging-hot-path.ko.md`를 뜻한다. `chand/report.md` 등 job 경로는 `/root/jobs/fx/cx/v21/s033/` 기준이다. 파일 전체와 줄 번호를 `logs/read-first.txt`, `logs/spec-sources.txt`, `logs/implementation-sources.txt`에 보존했다. .NET 원본과 diff는 `logs/dotnet-6821c3b8af.cs`, `logs/dotnet-6821c3b8af.diff`에 있다.

## 2. 적용 계약과 원문 인용

S08 §4.4 전체 규칙에서 이번 설계에 적용하는 구절이다. 표의 다섯 행도 각각 따로 인용했다.

| 항목 | 원문 인용 | 파일:줄 |
|---|---|---|
| 새 turn | “E5의 caller continuation과 I4 뒤의 handler turn은 **새 execution turn**에서 실행한다. 새 turn은 다른 thread를 뜻하지 않는다” | S08:302–303 |
| 수신 역할 범위 | “수신 역할이란 ingress owner로서 I0~I4와 관리 작업(§4.2)을 진행하는 일이며, E5의 완료 알림을 받는 binding poller 구동도 여기에 든다.” | S08:307–308 |
| 비수신 자원의 직접 실행 | “**수신 역할을 맡지 않은 실행 자원**은 Submit과 완료 「11」의 새 turn 조건을 지켜 그 turn을 직접 실행할 수 있다.” | S08:306–307 |
| 성공 조건 | “기존 실행 자원 배치의 소유자가 지금 비어 있는 자원을 다음 수신 역할에 독점 배정하고, 그 자원이 application queue를 기다리지 않고 수신을 진행할 수 있을 때만 성공이다.” | S08:310–311 |
| 거짓 성공 금지 | “Executor에 게시했다는 사실이나 waiter가 있다는 사실만으로 성공으로 판정하지 않는다.” | S08:311–312 |
| 이전 수신 종료 | “이전 자원의 wait·drain과 receive turn이 끝난 뒤 넘겨받은 자원이 같은 poller의 다음 수신을 시작한다.” | S08:312–313 |
| 역할 수명 | “Socket·poller·cursor·반환되지 않은 permit과 shutdown 수명은 실행 자원이 아니라 수신 역할에 속한다.” | S08:313–314 |
| 실패 경로 | “배정을 보장할 수 없으면 인계하지 않고, 현재 자원이 수신 역할을 유지하며, turn은 owner queue와 completion dispatcher에 남아 다른 실행 자원(worker)이 실행한다.” | S08:314–316 |
| 표: 진행 영역 분리 | “수신 역할을 넘기지 못한 자원은 application 작업을 실행하지 않는다.” | S08:322 |
| 표: 순서와 실행 권한 | “배치만으로 실행 권한을 얻거나 queue 순서를 건너뛰지 않으며, application 문맥에는 기존 진입 경로로 들어간다” | S08:323 |
| 표: callback 독립 | “서로 무관한 완료 callback을 한 callback의 반환에 의존하는 drain으로 합치지 않는다. 언어 runtime이 소유한 worker 관리 정책과 application이 고른 실행 context는 유지한다.” | S08:324 |
| 표: 회전 단위 | “잘못 바꾼 인용: 인계는 수신 회전이 끝난 경계에서 한 번 판단한다. Record마다 인계하지 않으며 I2의 연속 claim과 회전당 관리 작업을 그대로 둔다.” | S08:325 |
| 표: 자원 수 | “인계를 위해 실행 자원을 새로 만들거나 상한을 늘리지 않는다. 넘겨받은 자원이 실제로 시작했는지 확인하는 대기를 두지 않는다. Socket마다 동시에 수신을 진행하는 자원은 하나다.” | S08:326 |
| 비용·단일 runtime | “인계 성공 횟수는 전환 수와 따로 기록한다”; “인계와 통지에 드는 비용은 측정에서 빼지 않는다”; “실행 자원이 하나뿐인 runtime(Node의 event loop)에는 넘겨받을 자원이 없으므로 수신 역할을 맡은 자원은 항상 기존 경로를 쓴다.” | S08:328–332 |
| 내부 확인 | “handler나 continuation을 붙잡아 둔 상태에서도 넘겨받은 자원의 수신·completion·관리 처리가 그 해제 전에 진행하는지”; “배정된 자원이 application 작업에 다시 배정되지 않는지”; “socket별 동시 수신 자원 수가 최대 1인지” | S08:334–336 |
| 자원 상한 확인 | “인계 성공·실패 횟수, 인계 전후 실행 자원 high-water가 기존 상한 이하인지는 trace로 확인” | S08:336–338 |

진입과 통지에 대한 추가 경계는 다음과 같다.

- S04:116–125: permit 취득 → receive → owner queue로 permit 이전 → callback 실제 첫 instruction 직전 반환이다. 미사용 rotation budget 반환과 record의 permit 반환을 혼동하지 않는다.
- S04:194–200: gate 없는 owner는 미취득 record가 남는 동안 ready이며, head take는 owner 독점 권한이 아니다. S04:202–218: idle 등록 뒤 ready 재확인, 미예약 idle에만 배정, publication/idle 경계에서 통지 병합, 수신 역할 자원 제외, scheduler 구간 밖 통지, record별 task 금지다.
- S08:183–187,197–205,220–247: I0 대기·I2 최대 64건 연속 claim·회전 관리·W1~W5와 시간 예산을 유지한다. `cpath/report.md:162–178,296,301`의 R1~R10·회전 분담 중 이번 실험은 **배치·통지 비용만** 바꾼다. decode·scope·handler·reply·terminal 비용은 삭제하지 않는다.
- S02:94–108,113–131: Node/Channel은 FIFO take와 record별 독립 turn, Spot/Actor는 기존 gate, 일반 await는 권한 유지, Yield만 기존 queue를 통한 권한 재획득이다.
- S02:490–515: payload decode·user callback은 application turn이며, application이 기다려도 infrastructure가 진행해야 한다. Thread 이름 변경만으로 application 문맥에 진입했다고 볼 수 없다.
- S01:492–519,539–544: completion 잠금과 게시 stack·lane-current scope가 끝난 새 turn, 독립 callback·기존 dispatcher·수락 callback drain·exception 격리를 유지한다. **이번에는 source completion 배치나 Asio pool을 수정하지 않는다.**
- `doc/principal/dev/zlink-system-design-principles.ko.md:87–105`: Framework 자원 배치만 변경하고 binding/Core의 poller drain·재제출·native reply routing을 재판정하지 않는다. 대안 분리와 제한 실험은 `doc/principal/dev/posddd.ko.md:539–544,730–751`을 따른다.

## 3. 현재 C++ 결정 경로

다음은 hosted Mesh target의 수신 역할·application 배치에 관여하는 경로 전부를 책임별로 나눈 표다. 전달 함수와 결정 소유자를 구분한다. Standalone/manual ingress와 별도 Channel/STREAM receiver를 이번 target 최적화의 대상으로 확대하지 않는다(C/stateful/public_host_runtime.cpp:7483–7490,7493–7515; C/mesh/raw_mesh_node_owner.cpp:4258–4261).

| 결정·경계 | 현재 파일:줄 | 판정과 변경 경계 |
|---|---|---|
| Pool 수와 idle retirement | C/mesh/mesh_node_host_service.cpp:844–849; C/dispatch/offload_executor.cpp:56–72,102–107,163–168,273–298,335–358 | 생성 시 최소 `등록 수+1`, 최대 `등록 수+max(2,CPU 수)`, idle 100 ms. 일반 작업의 기존 증원·retirement 정책은 유지하며 인계는 생성·join을 호출하지 않는다. |
| 초기 수신 역할 요청 | C/mesh/mesh_node_host_service.cpp:3363–3364,3455–3456; C/dispatch/offload_executor.cpp:172–181 | host가 role을 만들고 executor `_roles`에 게시한다. 여기서 특정 worker는 정해지지 않는다. |
| 실제 역할/작업 worker 선택 | C/dispatch/offload_executor.cpp:330–370,373–391; C/dispatch/offload_executor.hpp:102–132 | shared mutex 아래 `_roles` 우선, 그 다음 `_queue`. `_idle_workers`는 개수이며 독점 예약이 아니다. `role`/`work`는 worker-loop local이다. |
| 수신 역할 유지·회전 반복 | C/mesh/mesh_node_host_service.cpp:2158–2167,2410–2413 | `run()`이 종료까지 반복하고 readiness는 그 stack local이다. 인계 시도 없음. 회전 제어를 executor 경계로 옮겨야 하는 지점이다. |
| Role이 보유한 수명 | C/mesh/mesh_node_host_service.cpp:2145–2154,2171–2186,2410–2413 | node·registration·receive_permit·supply·pending_management를 같은 role에 유지한다. Readiness만 run-local에서 같은 role로 이동한다. |
| 회전 permit·mailbox publication 범위 | C/mesh/mesh_node_host_service.cpp:2188–2207,2353–2356 | permit batch 예약, receive turn 시작, 종료하며 ready batch를 분리하고 미사용 permit을 반환한다. |
| 실제 poll/claim 순서 | C/stateful/public_host_runtime.cpp:7483–7490,7518–7537; C/mesh/raw_mesh_node_owner.cpp:3362–3390,3593–3606 | hosted dispatch가 한 receive-turn port lease·poll 뒤 연속 receive한다. Raw owner는 자원을 배정하지 않는다. |
| Binding poller와 socket 진입 | C/backend/raw_route_port.cpp:418–465,473–482 | 기존 poller mutex와 socket mutex. 이 lock으로 두 receiver를 허용한 뒤 직렬화하는 설계는 금지한다. Worker 역할 배정에서 하나만 허용한다. |
| Application 분류·owner commit | C/mesh/raw_mesh_node_owner.cpp:4143–4226; C/mesh/service_mailbox.cpp:133–168 | raw ingress가 Node/Channel/Spot/Actor owner를 정하고 canonical mailbox에 commit한다. ready 집합의 소유자는 mailbox다. |
| Hosted callback의 별도 경로 | C/mesh/mesh_node_host_service.cpp:2211–2242,2264–2348 | actor transfer·infrastructure direct dispatch·application local reenqueue 경로가 있다. 이 경로들을 통째로 application callback으로 바꾸지 않는다. Application user entry가 있는지는 §8 T06에서 별도 확인한다. |
| Ready batch 생성·통지 | C/mesh/service_mailbox.cpp:539–589; C/mesh/mesh_node_host_service.cpp:2353,2398 | receive turn 중 통지를 보류하고 ready owner 목록을 swap한다. 각 owner callback을 호출하며 record마다 제출하지 않는다. |
| Host의 handler 배치 요청 | C/mesh/mesh_node_host_service.cpp:3356–3361,2125–2139 | `make_application_owner_work` → `try_submit_internal`; 거절이면 `start(true)` 정리 경로다. 정상 handler를 수신 역할에서 실행하는 분기는 없다. |
| 관리·다음 대기 결정 | C/mesh/mesh_node_host_service.cpp:2357–2399; C/mesh/raw_mesh_node_owner.cpp:4254–4286; C/stateful/public_host_runtime.cpp:8595–8613 | 관리 activity 상태, permit supply 재확인, 다음 deadline까지 blocking readiness wait. 새 worker 시작 확인 대기와 다르다. |
| Owner drain 권한·claim | C/mesh/mesh_node_host_service.cpp:3861–3917; C/mesh/service_mailbox.cpp:507–536,200–237; C/stateful/public_host_runtime.cpp:8528–8564 | mailbox phase와 prefix claim, 기존 10 ms W5 예산. 배치 최적화가 이 권한을 대체하지 않는다. |
| Application 진입·permit·reply | C/mesh/mesh_node_host_service.cpp:2134–2136,3779–3832; C/channels/route_handler_invoker.cpp:26–49,63–95; C/mesh/mesh_record_dispatcher.cpp:53–71 | 기존 ambient 진입과 dispatch를 재사용한다. Terminal owner의 `enter_handler`는 C/mesh/application_dispatch_terminal_owner.hpp:37–40이다. Reply는 completing handler에서 기존 token으로 binding submit한다. |
| Async 수명·종료 | C/dispatch/offload_executor.cpp:111–148,184–194,218–258,392–406; C/mesh/mesh_node_host_service.cpp:3705–3713,3901–3904 | retained credit·capture 파괴·application drain → receive stop → 전체 drain 순서를 보존한다. 인계는 role 종료가 아니므로 `supply.close()`나 role credit 반환을 하지 않는다. |
| Logical completion 보호 경계 | C/foundation/operation_registry.cpp:129–136,269–285,539–552; C/dispatch/provider_task_context.cpp:174–191 | 기존 C1 take와 독립 process completion dispatcher. Target 인계 때문에 이 경로를 합치거나 local callback drain으로 바꾸지 않는다. |

“각 record가 다른 worker로 간다”는 설명은 현재 코드의 보장과 다르다. 현재는 **ready owner별 drain 제출**이고 그 drain이 여러 record를 처리할 수 있다(C/mesh/service_mailbox.cpp:570–575; C/mesh/mesh_node_host_service.cpp:3356–3361,3894–3909). Packet 차이와 target CPU는 관측됐지만, handoff가 packet 차이의 유일 원인이라는 인과 검증은 없다(`creply/report.md:1–3`; `cadvice/report.md:137–139`).

## 4. 언어별 대조

| 언어·기준 | Receive / application 배치의 대응 위치 | 이번 설계에 적용하는 판단 |
|---|---|---|
| .NET, **6821c3b8af** | D@6821/Execution/ZLinkProcessExecutionPool.cs:307–335의 `DispatchApplicationsUnderLock`; :400–415에서 `Receive()` 및 `_receiving` scope 반환 후 호출; :456–467에서 기존 ready source를 선택; :715–723 worker의 role/idle/application 상태 | :313–321은 **idle list가 아니라 worker 배열에서 아직 소비하지 않은 `Application` 예약을 찾아 교환**한다. `currentWorker.Application=application`, donor의 application 제거, receiving 교환, 특정 worker 통지다. 현재 C++의 queued post보다 강한 배정 근거지만, 이번 요청의 “미예약 idle만 사용”과 동일하지 않다. |
| .NET, 같은 커밋 | 같은 파일 :358–376,411–412는 `lock(_gate)`; :325–335는 idle list에서 application 예약; :1045–1061은 정상 application turn; :63–66은 자원 상한 | .NET은 non-waiting try-lock을 입증하는 선례가 아니다. Worker 배열 검색·blocking lock·예약 뺏기를 그대로 복제하지 않는다. 새 C++ 역할 배정은 try-lock 1회와 기존 idle membership의 한 원소 선택으로 제한한다. |
| Java core | J/binding/ZLinkJavaRawMeshNode.java:5592–5605 전용 platform single-thread pump, :5613–5647 회전·재개, :5674–5826 permit·연속 claim·state lane 밖 dispatch | 수신 역할을 application worker에 넘기는 경로가 아니라 고정 pump와 application 경로 분리다. Kotlin은 이 Java core를 공유한다(`framework/languages/java/zlink-framework-kotlin/build.gradle.kts:13`). |
| Java application | J/binding/ZLinkJavaRawMeshNode.java:6253–6293 direct application receiver 또는 application executor; J/channels/ZLinkMeshApplicationDispatcher.java:1020–1026 namespace queue; J/channels/ZLinkHandlerDispatch.java:16–44,47–60 queue에 pipeline start를 게시하고 async terminal은 분리 | 자원 정책은 다르다. Java의 queue/future 구조를 C++ per-record executor 재제출의 근거로 삼지 않는다. 공통 FIFO·gate·progress 결과만 대조한다(S02:97–108; S08:243–251). |
| Node | N/foundation/raw-service-mesh-runtime.ts:1021–1075 회전; N/backend/node/node-raw-mesh-backend.ts:1855–1884 ready publication·completion microtask; N/backend/mesh-dispatch-pump.ts:94–103,119–177,291–330 application/infrastructure scheduling | idle “application worker” 배열은 JS 실행 단위를 기다리는 continuation이다. 별도 OS receive worker를 뜻하지 않는다. S08:330–332에 따라 기존 event-loop 경로 유지이며 C++ 성공률과 같은 지표를 요구하지 않는다. |

물리 배치 차이는 S08:306–332가 이미 허용한다. 새 public 결과, FIFO 예외, gate/permit 면제는 제안하지 않는다. 따라서 그 자체는 D(spec gap)가 아니다. 반면 Node/Channel 독립 실행의 기존 언어 차이는 동일 계약에 대한 구현 대조 항목으로 남겨야 하며, C++ prefix 차단을 정상 계약으로 인정할 근거가 되지 않는다(S02:97–104; `p2/report.md:13–23`).

## 5. 소유자와 규칙 수

### 결정 소유 표

숫자는 branch 수·호출 수·mutex 수가 아니라 **해당 결정을 최종 판정하는 코드 위치 수**다. 같은 결정의 단순 전달·통지는 별도 판정 위치로 세지 않는다. 동일 조건을 두 함수가 다시 검사하면 두 위치로 센다(`chand/report.md:11,25,112`). 이번 범위의 물리 자원 배치는 기존 `offload_executor_t` 하나가 계속 소유한다.

| 결정 | 소유 조항 | 현재 판정 위치와 수 | 제안 후 단일 소유자와 수 |
|---|---|---|---|
| 수신 역할의 worker 선택·유지 | S08:309–316,325–326 | executor 최초 선택 C/dispatch/offload_executor.cpp:360–363 **1** + host의 종료까지 고정 유지 C/mesh/mesh_node_host_service.cpp:2158–2163 **1** = **2** | executor의 회전 반환 후 동일 worker-assignment 경계 **1**. host는 한 회전 결과만 반환한다. |
| Application drain의 worker 선택 | S08:323; S04:202–218 | C/dispatch/offload_executor.cpp:360–370 **1**. Host :3360은 게시 요청이며 worker 선택자가 아니다. | 같은 executor worker-assignment 경계 **1**, normal queue head만 선택한다. |
| Idle 가용성·예약 | S04:202–218 | C/dispatch/offload_executor.cpp:335–370 **1**; 현재 표현은 개수+공용 wait | 같은 executor membership/assignment 경계 **1**, 수신 역할 예약 자원 제외 |
| Canonical owner ready 상태 | S04:194–200,235–242 | C/mesh/service_mailbox.cpp:133–168,456–479,507–567의 ready 삽입(:161–163), prefix 종료(:471–476), drain 종료(:525–531), ready batch take(:561–566)의 판정 위치 **4** | 같은 mailbox의 **4**, executor에 ready 사본 없음 |
| Application drain 진입 권한 | S08:323; S02:94–108 | C/mesh/service_mailbox.cpp:507–514 **1** | 같은 소유 **1**; 새 receiving flag를 gate 대용으로 쓰지 않음 |
| FIFO prefix claim 권한 | S04:194–200 | C/mesh/service_mailbox.cpp:212–217와 :235–237의 중복 active-prefix 검사 **2** | 같은 **2**. 이번 실험에서 제거하지 않으며 현재 독립성 문제는 §8의 별도 차단 조건 |
| W5 budget 종료 | S08:224 | C/mesh/mesh_node_host_service.cpp:3879,3908–3909 **1** | 같은 drain **1**, 역할 교환으로 budget 재설정하지 않음 |

이 표의 결정별 합은 **12 → 11**, 물리 배치 모듈 소유자 수는 **1 → 1**이다. 첫 행 외의 동작은 그대로이며, 첫 행의 host 고정 유지 판정만 executor로 옮긴다. 역할·application·idle 세 결정은 변경 후 하나의 상태 변경 구간에서 처리한다. Logical C1·binding completion authority·Spot/Actor gate는 별개 결정이며 이 합계에 포함하지 않는다(§3의 보호 경계; S08:323–324).

**실제 변경 전/후는 8 → 8**이다. 아직 구현하지 않았으므로 제안 수를 완료 결과로 보고하지 않는다.

### 상태 표현의 교체

새 권위 상태를 기존 상태 옆에 더하지 않는다. 다음은 구현 승인 뒤 확인할 삭제/이동 조건이다(C/dispatch/offload_executor.hpp:102–132; C/dispatch/offload_executor.cpp:330–406).

| 제안 표현 | 함께 없어지는 표현·범위 |
|---|---|
| Worker별 안정된 assignment slot. 기존 worker-loop local `role`/`work`를 slot으로 이동하고 idle/role/work 중 하나만 소유 | local role/work를 병행 보관하지 않는다. `receiving`, `reserved`, `started`, generation 같은 별도 bool/counter는 추가하지 않는다. |
| Worker의 intrusive idle membership, 특정 worker를 깨우는 대기 객체 | `_idle_workers` 숫자와 공용 `_ready` 대기/통지 표현을 교체한다. Assignment를 받는 즉시 idle에서 제거한다. 기존 `_mutex` 하나를 유지하며 새 lock·timer는 없다. |
| 기존 작업 queue 한 개의 item이 초기 role 또는 application work를 보관 | 별도 `_roles` queue를 제거한다. 초기 role 우선/FIFO는 같은 queue의 role 구간으로 유지하고, application FIFO는 그대로다. 초기 role 삽입은 시작 시 기존 role 구간 끝에 넣는다. Hot handoff는 이 queue에 role을 넣지 않고 idle slot에 직접 배정한다. |
| Readiness와 회전 결과의 기존 ready batch를 role 수명으로 이전 | `run()` stack의 readiness와 `rotate()`의 ready batch를 중복 복사하지 않는다. Record·payload는 기존 mailbox에 남는다. 별도 record queue·completion queue·handoff broker는 없다. |

작업 queue의 item 표현 변경과 worker-slot 수명은 실험에 필요한 내부 배치 경계다. Mailbox의 prefix/settlement 제거, persistent multi-drain source 도입, source Asio 통합, executor 전면 재작성은 이 실험의 범위가 아니다(`cadvice/report.md:297–303,325`; `p2/report.md:23`).

## 6. 제안 알고리즘

### A. 선택안: 회전 반환 후 idle worker 독점 배정

1. **기존 rotation을 유한하게 반환시킨다.** 현재 host `run()`의 종료까지 반복을 executor worker loop로 옮긴다. Host role은 같은 I0~I4·관리 및 필요한 readiness wait를 수행하고 한 회전이 끝나면 반환한다. 기존 `pending_management`, supply와 permit은 role에 남긴다. `supply.close()`는 실제 수신 종료에만 실행한다(C/mesh/mesh_node_host_service.cpp:2158–2163,2188–2207,2353–2413).
2. **수신/게시 stack을 먼저 끝낸다.** `dispatch_hosted_ready`, binding wait/drain, mailbox receive turn, 미사용 budget 정리, 관리 확인이 끝난 뒤 기존 ready-owner batch를 executor 경계의 local로 move한다. Successor에게 role을 배정하기 전에 이 batch를 role에서 완전히 분리하므로 다음 회전과 같은 batch를 동시에 쓰지 않는다. 현재 `notify_application_ready(ready)` 호출을 role의 회전 밖 게시 경계로 이동한다. Payload decode·handler 호출은 하지 않는다. `rotate()` destructor나 `terminal_callback_guard`에서 실행하지 않는다(C/mesh/mesh_node_host_service.cpp:2205–2207,2353–2399; S08:312–323; S01:508–513).
3. **기존 owner drain만 게시한다.** `make_application_owner_work`와 같은 owner별 work를 기존 executor queue에 게시하고, rotation의 알림은 게시가 끝난 경계에 합친다. `try_submit_internal`의 정상 수락·종료 판정과 queue lifetime을 재사용한다. 새 record별 callback/task는 만들지 않는다. 게시 자체의 기존 mutex/할당 비용을 0으로 주장하지 않는다(C/mesh/mesh_node_host_service.cpp:2125–2139,3356–3361; C/dispatch/offload_executor.cpp:92–108; S04:216–218).
4. **추가 인계 판정은 회전마다 최대 한 번, `try_lock` 한 번이다.** 기존 executor mutex가 즉시 잡히지 않으면 인계 실패다. 잡힌 경우에도 executor가 stopping, 이미 배정된 idle 자원뿐, 새로 만들어야 하는 자원뿐, 실행할 eligible owner drain이 없으면 실패다. 재시도·spin·wait·worker 검색·retired-thread join은 없다. 이미 존재하는 idle membership의 head 하나만 본다. 기존 application 게시 경로의 잠금과 별개로 **인계가 추가하는 잠금 대기 0**을 시험한다(S08:310–316,325–326; `chand/report.md:67,71,94,102`).
5. **성공은 role과 기존 queue 작업의 원자적 교환이다.** 동일 executor 구간에서 idle successor를 membership에서 제거하고, 현재 role의 유일 assignment를 successor로 move한다. 이전 worker는 기존 application queue의 정상 head 하나를 take한다. Successor의 slot에는 application work가 들어갈 수 없다. Queue 선행 work를 건너뛰어 이번 rotation의 record를 우선시키지 않는다. 선행 work가 없으면 이전 수신자가 자기 rotation의 owner drain을 실행하며, 선행 work가 있으면 그 순서를 따른다. 자원 친화성은 FIFO를 바꾸는 보장이 아니다(S08:310–316,323; C/dispatch/offload_executor.cpp:360–370).
6. **Lock 밖에서 successor와 독립 작업의 통지를 보낸다.** 역할 assignment 완료 뒤 특정 successor에 한 번 통지한다. 이미 배정한 worker에 중복 통지하지 않는다. 나머지 owner work는 기존 worker 정책으로 독립 실행 가능하게 먼저 공개/예약한다. 그 다음에 이전 worker가 기존 `run_application_turn` → `make_application_owner_work` → `drain_application_owner` → claim/gate/permit → dispatch 경로에 들어간다. 배정된 successor의 첫 instruction을 확인하거나 기다리지 않는다. 이전 receiver의 마지막 수신 접근과 다음 receiver의 첫 접근은 assignment mutex의 release/acquire로 순서를 만든다(C/dispatch/offload_executor.hpp:32–40; C/mesh/mesh_node_host_service.cpp:2125–2139,3861–3917; S04:216–218; S08:312–326).
7. **이전 worker는 owner drain의 기존 W5만 실행한다.** 동기 echo는 기존 drain budget 안에서 여러 record를 처리할 수 있으나 batch를 채우려고 기다리지 않는다. Handler suspend/terminal 이후는 기존 continuation·retained credit이 소유한다. Source completion callback들을 이 drain에 포함하지 않는다. Reply encode/submit은 각 handler 완료 turn의 기존 W4다(C/mesh/mesh_node_host_service.cpp:3879–3917; C/channels/route_handler_invoker.cpp:47–95; S08:222–247).
8. **실패하면 role을 그대로 유지한다.** 어느 실패에서도 local application work를 take하지 않는다. 이미 게시한 owner work는 normal queue에 남겨 기존 worker 배정/통지를 수행하고 현재 worker가 다음 rotation을 진행한다. Try-lock 실패 뒤 같은 rotation의 인계 시도를 다시 하지 않는다. 정상 publication에 필요한 기존 잠금은 원래 경로대로이며, 이를 인계 성공이나 무잠금 경로로 세지 않는다(S08:314–316; `chand/report.md:102,105`).

3~6의 publication과 idle 재확인은 **하나의 기존 scheduler 알고리즘**이다. Publication 뒤 알림 전 worker가 idle 경계에 도착해 ready queue를 먼저 가져가도 안전해야 한다. 이 경우 local work나 successor를 확보하지 못하면 실패 경로를 쓴다. 알림 보류를 위해 `handoff_pending` flag나 별도 queue를 추가하지 않는다. 게시된 work/worker assignment 자체가 수락·예약의 권위 상태다(S04:202–218). Queue publication 실패 시 아직 수락되지 않은 owner 알림을 소모하지 않도록 기존 ready batch의 항목을 수락 후에만 제거하고, 수락한 항목은 기존 drain credit으로 끝까지 보존해야 한다(`chand/report.md:108`; C/mesh/service_mailbox.cpp:570–575는 현재 pop-before-callback이므로 승인 뒤 예외 시험 대상이다).

### 종료·예외

- Role credit는 이동 시 **반환/재취득하지 않는다**. `_active`·`_role_active`의 기존 의미를 유지하고, 이전 worker가 take한 application work만 기존 active accounting으로 센다. Queued/assigned role과 queued/running/retained application 모두 기존 drain 판정에 포함한다(C/dispatch/offload_executor.cpp:114–148,193,235–245,301–305,360–370,392–406).
- Stop이 배정 전이면 실패, 배정 후면 successor가 기존 role stop 경로를 끝낸다. `drain_application()` 동안 수신을 유지하고, host `_stop` 설정·wake 뒤 수신 종료와 전체 drain을 기다린다. 다른 worker로 옮겼다는 이유로 role supply나 socket을 닫지 않는다(C/mesh/mesh_node_host_service.cpp:3705–3713; C/dispatch/offload_executor.cpp:184–194).
- Receive exception은 기존 terminate 정책, application exception은 기존 terminal/error mapping 경로다. 인계 실패를 exception 삼키기로 처리하지 않는다(CT/test_cpp_framework_offload_executor.cpp:256–268; C/dispatch/offload_executor.cpp:373–391; C/mesh/mesh_node_host_service.cpp:3842–3857).
- Accepted work 준비/게시 중 실패, assignment 직후 close, callback/준비 예외는 동일 record/operation을 기존 소유자로 정리한다. 새 completion flag·fallback operation·새 retry는 금지한다(S01:521–544; `chand/report.md:108`).

### B. 비교안: 수신 역할 유지와 기존 owner drain 게시

기존 경로는 가장 작은 변경량(제품 변경 0)이지만 target receive→application 배치를 유지한다(C/mesh/mesh_node_host_service.cpp:2161–2162,2398,3360). 이를 필수 실패 경로로 유지한다. 반대로 고정 receive thread에서 handler를 직접 호출하는 방식은 긴 동기 handler가 수신을 멈추므로 탈락이다(S08:309–326; `cadvice/report.md:220–225,245`).

A는 회전 끝 역할의 판정 위치를 합치며, B는 성능 실험의 base다. Cpath §7의 prefix 제거와 scheduler 개편을 A에 섞으면 handoff의 효과를 분리할 수 없으므로 제안하지 않는다(`cpath/report.md:296,301`; `cdrain2/report.md:9–12,163–174`).

## 7. 계약별 검증 매핑

시험 ID는 다음 절의 계획이며 실행 결과가 아니다.

| 필수 계약 | 보존 방법·경계 | 입증 시험 |
|---|---|---|
| S08 표: 진행 영역 분리 (:322) | 실패 시 수신 자원 user entry 0, 성공 후 successor가 같은 poller와 관리 상태 소유 | T01, T02, T04, T06 |
| S08 표: 순서·실행 권한 (:323) | 기존 owner queue→drain→claim/gate→permit 반환→application entry; 배정 자체는 권한 아님 | T05–T07 |
| S08 표: callback 독립·runtime/context (:324) | source dispatcher 불변, siblings는 local handler 전에 공개; caller scheduler 유지; 전체 application worker 동기 block 시 새 callback 진행까지 약속하지 않음 | T04, T05, T08 |
| S08 표: 회전 단위 (:325) | rotation 반환 경계에서 try-lock 1회 이하, I2/관리 안에서는 시도 0 | T03, T10 |
| S08 표: 실행 자원 수 (:326) | 이미 생성·idle인 slot만 move; handoff 경로 create/join/ack 대기 없음; 유일 role slot | T01–T03, T09 |
| S08 역할·수명 (:312–316) | wait/drain/receive turn 반환 후 role 전체 이동; 실패 시 role/record 소유권 불변 | T02, T06, T09 |
| S08 지표·단일 runtime (:328–338) | handoff·record transfer·application wake·receiver wake·high-water 따로 집계; Node 기존 경로 | T03, T10, T12 |
| chand §4: infrastructure 독립 (:102) | local A를 실제로 block하고 successor의 새 TCP request·completion·관리 진행을 A 해제 전에 관측 | T04. 단순 `signal_activity`만으로 합격하지 않음 |
| chand §4: queue/gate/permit (:103) | 보통 await와 Yield의 captured scheduler·gate 유지/재획득, permit handler 직전 반환 | T06, T07, T12 |
| chand §4: callback 독립 (:104) | unrelated completion B 및 다른 owner B는 A block 중 실행. 같은 Node B도 별도 필수 조건이며 현재 blocker | T05, T08 |
| chand §4: 회전 단위 (:105) | attempts≤rotations; management와 연속 claim budget 불변 | T03, T10 |
| chand §4: 자원·ack·동시 수신 (:106) | targeted assignment, max receiver 1, high-water 기존 상한, successor를 일부러 지연시켜도 이전 자원이 ack를 기다리지 않음 | T01, T03, T09 |
| chand §4: terminal·exception·수락 수명 (:108) | 기존 C1 slot·reserved completion item·physical lifetime 불변; accepted work를 재생성/폐기하지 않음 | T08, T09, T11 |

Source 연구의 “local completion 최대 한 개”(`chand/report.md:69`)를 target의 “handler 한 개만 실행”으로 바꾸지 않는다. 이 target은 기존 **owner drain 한 turn**을 선택하며 그 turn은 W5 안에서 여러 record를 처리한다. 서로 무관한 completion callback을 직렬 drain으로 합치지 않는 조건은 별도로 유지한다(S08:224,243–247,324).

## 8. 시험 계획과 선행 차단 조건

아래 **12개 검증 항목**, 양성·음성 대조를 승인 뒤 작성/실행한다. 임의 timeout 증가는 없고 deterministic barrier로 관측 순서를 만든다. 표의 파일:줄은 기존 시험의 시작점 또는 동작 근거이며 제안한 시험이 이미 존재한다는 뜻이 아니다.

| ID | 양성 조건과 개수 | 음성 대조·기존 근거 |
|---|---|---|
| T01 idle assignment | 미리 생성된 worker 3개, role 1개. idle 1개에만 exclusive assignment; successor app take 0; former receiver local drain 시작 1; new thread 0 | queued-role post만으로 성공을 기록하는 변형은 실패해야 한다. CT/test_cpp_framework_offload_executor.cpp:152–173,277–288 |
| T02 비대기 실패 | scheduler mutex 경합, idle 없음, 전부 예약됨, stop, ready 없음의 5조건. 각 조건에서 추가 인계 시도 1 이하, 성공 0, local user entry 0; record와 role 유지 | mutex 소유자의 release 전에 try-assignment가 반환함을 barrier로 확인. blocking lock/ack 변형은 실패. C/dispatch/offload_executor.cpp:330–370 |
| T03 receiver·회전 계수 | 64-record 회전과 빈 회전, 반복 role 교환에서 socket별 poll+drain+claim 동시 실행 최대 1; attempts≤rotations, attempts=success+failure, role 이동 때 active role 총수 불변 | 두 worker에 같은 role을 배정하거나 record마다 시도하는 변형 검출. CT/test_cpp_framework_receive_role_completion.cpp:138–164; C/dispatch/dispatch_limits.hpp:10–15 |
| T04 block 중 실제 진행 | 성공 후 former receiver의 handler A 동기 block. 해제 전 successor가 새 TCP request 수신, 별도 outbound request reply의 C1 완료, 관리 deadline/peer admission 각각 1회 이상 관측 | 수신 역할을 유지한 채 A를 실행하는 변형은 진행 검사가 실패. 단순 역할 TID 변경만으로 합격 금지. `chand/report.md:102`; CT/test_cpp_framework_receive_role_completion.cpp:166–184 |
| T05 handler 독립·FIFO | 다른 owner B, **같은 Node owner B**, 다른 gate B의 3조건에서 A 동기 block 중 B pipeline 진입. FIFO take sequence와 exclusive record ownership 유지; 같은 gate B는 진입 0 | 기존 prefix는 same-owner B를 막을 가능성이 높다. A terminal까지 B를 늦추거나 per-record task를 넣은 변형은 각각 독립성/게시 count 검사 실패. C/mesh/service_mailbox.cpp:212–237,507–514; `p2/report.md:9–23`; S02:97–108 |
| T06 기존 application 진입 | handoff 전 receive turn·poll/drain·lane scope depth 0, 정상 ambient 진입, decode·filter·handler에서 영역 올바름. Handler 첫 instruction 전 permit 반환 정확히 1, 취소/예외도 반환 1 | receive callback/rotation destructor에서 직접 handler 호출, permit 조기 반환 검출. CT/test_cpp_framework_handler_entry_permit.cpp:37–51; C/mesh/mesh_node_host_service.cpp:2134–2136,2264–2348 |
| T07 await/Yield | SpotWide/PerActor, ordinary await와 허용 Yield, Actor claim 유지·resume gate 재획득·같은 gate overlap 0; application-selected scheduler 보존 | gate 우회·Yield inline 재개 변형 검출. CT/test_cpp_framework_yield_admission.cpp:89–145; 소유 계약 S02:94–131 |
| T08 completion 독립 | target의 outbound operation completion A block 중 unrelated completion B 진행, 각 1회. 이미 수락한 completion은 callback capture 파괴까지 drain에 포함 | batch callback 직렬 loop로 바꾼 음성 대조 검출. C/dispatch/provider_task_context.cpp:174–191; CT/test_cpp_framework_receive_role_completion.cpp:185–229; CT/test_cpp_framework_completion_dispatcher_owner.cpp:14–40 |
| T09 shutdown·lifetime | idle→reserved→role 실행 전/후, ready batch 미게시, work queued/running/suspended 각 경계 stop. 받은 record terminal/정리/permit 1회, role supply close 1회, accepted work 0 유실 | role 이동 때 close, role credit 이중 감소, capture 파괴 전 quiescence 변형 검출. C/mesh/mesh_node_host_service.cpp:3705–3713; CT/test_cpp_framework_offload_executor.cpp:176–205 |
| T10 batching·통지 | 64 ready records, 여러 회전, last-take/enqueue·idle/publication 경쟁. 인계 통지 성공당 1 이하, 중복 예약 0, record별 executor task 0, 관리 확인 회전당 1, ready 유실 0 | take/enqueue마다 notify, 모든 worker wake, 새 supervisor task 변형 검출. W5 기존 budget을 같은 owner 재선택으로 갱신하는 변형도 실패. S04:202–224; S08:291–297; `p2/report.md:11` |
| T11 terminal·실패 주입 | reply/timeout/cancel/shutdown 경쟁에서 C1 승자 1·public delivery 1. Work 준비/게시/assignment 실패, handler/filter 예외에서도 수락 record 정리·reply/error 또는 기존 종료 결과 1 | post 실패 뒤 catch-and-drop, 새 fallback operation 생성, duplicate terminal 변형 검출. `chand/report.md:108`; C/foundation/operation_registry.cpp:539–552; S01:521–544 |
| T12 parity·최종 gate | C++ target ↔ .NET/Java(Kotlin runtime 포함)/Node peers: count, 오류 종류, FIFO take 계약, gate, deadline, shutdown 결과 대조. C++ unit/contract 및 target_contract는 구현 안정 뒤 한 번 | 물리 TID 동일성은 C++ 전용 검사. 공통 결과 차이는 성능 합격으로 덮지 않음. `cadvice/report.md:314–319`; framework/AGENTS.md:5–13 |

**선행 차단 조건:** T05의 same-owner 조건은 현재 C++에서 이미 실패할 근거가 있다. T06의 actor-transfer/direct 경로도 user code를 실행하는지 실제로 구분해야 한다(C/mesh/mesh_node_host_service.cpp:2264–2348). 따라서 먼저 기준 동작을 공개 API repro와 기존 flow/file log로 확인한다. 실패하면 같은 #1639 안에서 소유 계층·분류 B를 PM에게 보고하고, **역할 배치만의 제한 실험과 전체 계약 충족 판정을 분리**한다. PM 승인 없이 prefix 제거·persistent source·새 wake 정책을 이 설계에 추가하지 않는다. 이것은 spec gap이나 새 이슈 제안이 아니다(`p2/report.md:13–23`; S02:97–104; S08:243–251).

## 9. 측정 계획과 중단 조건

### 같은 세션 base와 계수

관측 근거는 이전 세션이다. `creply/report.md:1`의 Framework data packet/request 0.886, binding 0.683–0.693는 **40k 구간**이고, `bstage/report.md:18–25,29–41`의 app≈22·receive≈15 µs/msg·target≈66–69 µs/msg·sendto≈0.79–0.83은 별도 구간이다. 특히 “22k” runner label과 실제 관측 32.0–32.4k msg/s를 같다고 쓰지 않는다. 이 값을 candidate의 사전 base로 대입하거나 packet/request를 sendto/msg로 대체하지 않는다.

승인 뒤 첫 실험은 같은 세션·같은 binary/package 설정의 **base/candidate/base/candidate/base/candidate**, 각 30초 warmup·20초 active, target 실제 완료 rate를 같게 맞춘 request-backpressure 1 KiB로 한다. 이 길이와 3쌍은 제안한 최소 실험 예산이며 이번 실행 결과가 아니다. psched8 launch·source CPU 0–7/target 8–15를 유지한다(`bstage/report.md:18,39`). Core/binding package와 바이너리 해시·로드된 library·pinning을 보존하고 배타 `/root/jobs/fx/cx/measure.lock`에서 측정한다(`cadvice/report.md:327`; `cdrain2/report.md:70,76`). 새로운 계측은 job-local/시험용으로만 두고 trace off 비교를 함께 낸다.

| 지표 | 정의·분모와 필수 원시 자료 |
|---|---|
| Handoff success/rotation | `H/R`; R은 전체 완료 rotation. eligible 회전 수 E, 시도 A, 성공 H, 실패 F도 별도 기록. `H≤A≤R`, `A=H+F`. Record별 성공으로 환산하지 않음(S08:325,328–329). 실패 이유는 계측용 분류로만 남기며 제품 제어 상태로 쓰지 않음. |
| Local 실행률 | 이번 rotation record 중 **실제 같은 자원의 다음 application turn에서 시작한 수 / handler 시작 수**. 성공해도 선행 queue work나 gate 때문에 local record가 없을 수 있으므로 H와 구별(§6.5; S08:323). |
| Target sendto/msg | capture 전체 target process·모든 TID의 sendto 수 / 같은 capture의 validated target request 완료 수. 기존 자료와 비교하기 위해 최다 sendto IO TID 값도 별도 제공(`bstage/report.md:18–25`). |
| TCP data packets/request | reply flow의 data packet 수와 bytes/packet. ACK·재전송·offload 조건을 고정하고 `creply` 방식 사용. sendto와 1:1로 해석하지 않음(`creply/report.md:1,19,80`). |
| App-worker + receive µs/msg | 역할은 TID에 고정되지 않으므로 회전·application entry/exit 경계의 자원 identity를 기록. 주 지표는 **해당 역할을 수행한 TID 합집합의 user+kernel CPU를 한 번씩 합산**해 같은 window count로 정규화. 역할별 CPU를 추가 낼 때는 phase 구간 attribution이 있어야 함. 양 역할을 한 TID가 수행하면 두 열을 단순 합산하지 않음. |
| TID 수명 보정 | before/after 합집합 사용; after-only TID는 after 값을 포함, before-only 수와 미관측 tail을 별도 보고. 교집합 집계 금지. 주기 표본과 start/exit 최종값을 보존하며 tail이 크면 비교 무효(`bstage/report.md:29–41`). |
| 총비용·상한 | target process 전체와 source caller/Core I/O µs/msg, app/receive 통지·futex·context switch 별도, receiver 동시성 max=1, live/owned thread high-water. 인계 통지 비용을 포함하며 단순 역할 재분류를 CPU 절감으로 세지 않음(S08:326,330–338; `cadvice/report.md:329–336`). |
| 검증 수 | submitted=completed=target received; errors/rejected/abandoned/validation failures=0; in-flight drain·혼입·drain bound 없음. 검사기는 정상 자료와 count/오류를 조작한 음성 자료를 모두 판정(`creply/report.md:82`; `cdrain2/report.md:207`). |

Role 표본과 queue publication→handler, successor notification→receive의 지연·실제 records/rotation·records/notification K를 수집한다. K=64나 이전 .NET handoff 지연을 대입하지 않는다(`cpath/report.md:172,176,249`; `cadvice/report.md:329–332`). 이 실험의 제품 runtime에는 성능용 flag·counter를 추가하지 않는다.

### 순서와 중단 조건

1. **정확성 우선 중단:** T01–T11 중 첫 실제 실패, receiver 중복, accepted work 유실, ack/lock 대기, resource 상한 증가를 발견하면 성능 실행을 멈춘다. 재실행 통과로 실패를 지우지 않는다(`chand/report.md:102–108`; AGENTS.md §4.1).
2. **배치 실험 중단:** 세 paired run에서 H/R 또는 실제 local 실행이 계속 0이면 인계 효과가 없는 것으로 중단한다. 성공이 있어도 receiver 통지를 합산한 총 publication/wake 비용이 base와 같거나 늘고, sendto/msg·역할 TID 합집합 CPU에도 일관된 감소가 없으면 전체 executor 개편으로 확대하지 않는다(`cadvice/report.md:336–338`). 임의의 성공률 목표값은 두지 않는다.
3. **개선 채택 조건:** 같은 rate 세 쌍에서 sendto/msg와 역할 TID 합집합 CPU의 candidate 중앙값이 모두 base보다 낮고, pair별 차이·범위를 같이 제시한다. 차이가 인접 base의 변동폭 이하면 효과 미확정으로 중단한다. Target 전체 CPU가 오르면 역할 이전만의 수치 개선으로 채택하지 않는다. 이 판정 기준은 이번 제한 실험의 제안이며 0.6/공통 S08 성능 합격을 뜻하지 않는다(`cadvice/report.md:323–338`; `cpath/report.md:72`).
4. **회귀 제한:** 위 조건을 통과한 뒤에만 request-backpressure·send-saturation 1 KiB 각 base/candidate 3회, 100 µs hot Node와 독립 Spot probe 100 ms cadence/250개 각 3회로 확인한다. 기존 실패 실험의 기준을 최소 회귀 제한으로 제안한다: 두 cheap-cell 처리량 중앙값은 base의 99% 이상, Spot p99 중앙값은 base의 2배 이하. 이는 `cdrain2/report.md:7–14,140–154`의 기준을 인용한 것이며 전체 계약 합격 기준을 대체하지 않는다. 하나라도 미달하면 중단하며 backlog 비교·helper 탐색·전체 wake·record별 wake를 추가하지 않는다.
5. **범위 제한:** Source caller나 Core가 병목인 상태에서 target 변화만으로 0.6을 예측하지 않는다. Source completion 통합·general executor rewrite는 새 진단과 PM 승인 대상이다(`cadvice/report.md:298–303,334–347`).

## 10. 감독자 전달 사항

PM에게 §5 결정 소유 표, §7 계약 매핑, §8의 기존 독립성 차단 조건과 §9 중단 기준을 함께 전달한다. 승인 요청의 대상은 **target rotation 배치의 A 실험 설계**이며, 같은 Node/Channel B 결함의 수정이나 성능 합격이 아니다. 승인 전 제품 변경·빌드·PR을 진행하지 않는다.

리팩토링 점검은 이 경로에서 성능/소유 중복 후보 3건(고정 role 유지 위치, 익명 idle/공용 wake 표현, owner prefix의 독립 실행 차단), 불필요 코드 확정 0건이다. 실제 수정 0건이다. 앞의 2건은 제안에 포함하고 prefix 문제는 같은 작업의 PM 차단 사항으로 남겼다. 새로운 spec·public API·binding 보상 경로는 제안하지 않는다(C/mesh/mesh_node_host_service.cpp:2158–2163; C/dispatch/offload_executor.hpp:116–132; C/mesh/service_mailbox.cpp:212–237).

운영 기록: 시작 점검에서 `bash scripts/dev/internal-docs.sh`를 1회 실행했다. 이 스크립트의 `git pull`/agent 정의 복사는 이번 요청의 GitHub 금지보다 우선하면 안 됐다(`scripts/dev/internal-docs.sh:8–18`). 이후 원격 작업·GitHub API/Issue/PR·push·sub-agent·다른 AI 도구는 사용하지 않았다. 이 예외와 원본 출력을 `logs/startup-exception.txt`에 기록했다.

음성 대조: C/does-not-exist.cpp:999999
