# q1645r — 수신 역할 인계의 설계 진단 (#1645)

## 1. 결과와 범위

**현재 구조(dispatcher 유지)를 추천한다. ②는 C++·Java의 Framework 소유 수신 경로에서 가능한 후속 설계이지만, 네 언어의 공통 개정안으로 지금 확정할 수 없다.** 주된 이유는 binding의 숨은 수신 thread가 아니다. 공통 binding 계약은 그런 thread를 금지한다. 실제 제약은 .NET binding Task의 비동기 게시, 기존 자원의 인계 예약 능력, callback 동기 대기, Kotlin dispatcher 보장이다.

②는 기존 자원 배치 소유자 하나에서 수신 역할과 업무 실행 역할을 교환하는 구조여야 한다. 다음 수신 task를 executor에 게시한 뒤 현재 thread에서 callback을 실행하는 것만으로는 인계가 아니다. 실행 가능한 수신 자원을 확보하지 못하면 수신을 application backlog 뒤로 미룬다. 새 pool·lock·queue·인계 flag를 기존 구조 옆에 추가하는 구현은 제안하지 않는다.

기존 측정도 90%를 보장하지 않는다. 110µs binding RTT와 65%라는 배경 가정에 C++ 두 구간 중앙값 합 34.816µs를 적용한 낙관 추정은 약 **81.8%**다. q1645·p1645 실제 계측 모집단에서는 각각 약 **46.6%·50.4%**다. 인계 비용 0, 다른 구간 변화 0을 가정한 참고 추정이며 측정 결과나 엄밀한 성능 상한이 아니다.

- Worktree: `D:/worktree/zlink-1645-completion-turn`.
- 실제 branch: `docs/1645-completion-turn`; HEAD: `c3d51cef991f8222b09394136076a3dafe8a84e0`. 브리프의 main 설명과 다르지만 지정 commit과 일치하며 읽기만 했다.
- 시작 시 저장소 변경 0개. 코드·test·spec·index·branch 변경, commit·push, build·측정·test, WSL 사본 생성, agent 실행 없음.
- 산출물은 이 보고서 하나다. 선행 진단의 조사·측정은 인용했으며 재실행하지 않았다.

### 근거 표기

경로는 worktree 상대 경로다. 추가 확인한 code line은 현재 HEAD다. 선행 보고서의 line은 **그 보고서 기준 revision**이며 현재 HEAD line으로 주장하지 않는다.

| 약어 | 경로 |
|---|---|
| C | `framework/languages/cpp/framework/` |
| D | `framework/languages/dotnet/src/Zlink.Framework/Runtime/` |
| J | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/` |
| K | `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/` |
| N | `framework/languages/node/packages/framework/src/runtime/` |
| S | `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md` |
| H / G / P | 같은 디렉터리 `08-messaging-hot-path.ko.md` / `02-handler-turn-and-execution-gate.ko.md` / `04-application-job-queue-and-backpressure.ko.md` |
| L | `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md` |
| BA / CP | `bindings/doc/spec/async-execution-model.ko.md` / `core/doc/spec/core/05-polling.ko.md` |
| qe / q / p / m | `D:/t/jobs/fx/cx/q1645e-report.md` / `q1645-report.md` / `p1645-report.md` / `m1639d2-report.md` |

qe의 기준 HEAD는 `129fcd735bfa50f5c65cf4fe2d03ba606265d04f`다. q·p profiler 값과 m 수정 revision을 현재 worktree 실행 결과로 취급하지 않았다. m:196 이후 “완료 실행 자원” 절이 앞부분의 단일 FIFO drain 설계를 대체하므로 최종 절의 결론을 사용했다.

## 2. 결정 소유와 언어별 실행 자원

### 2.1 결정 소유 표

| 결정 | 소유 조항 | 현재 판정 위치 | 인계안의 단일 소유자 |
|---|---|---|---|
| Native completion·HWM·payload 수명 | BA §4·§5(`67–98`, `102–125`), CP §4·§5(`129–160`) | 아래 언어별 public poller의 binding drain | Core/binding 유지. Framework가 native 결과를 다시 판정하지 않음 |
| Poller 구동·다음 수신 turn 배치 | H `26–27`, `50–53`, `177–187` | C host thread; D receive loop; J pump/Channel owner; N event loop | Framework의 기존 실행 자원 배치 소유자. Poller registration 유지 |
| Framework terminal 확정 | S §10, H `96–98` | C registry(qe C 표), D table/direct(qe D 표), J registry(qe J 표), N registry/table(qe Node 절) | 기존 registry/table 하나 유지 |
| 완료 callback 실행 자원·반환 회계 | S §11(`389–430`) | C registry dispatcher; D binding RCA/direct 또는 table dispatcher; J service dispatcher; N Promise reaction | 자원 통합이 가능한 경우만 기존 배치 소유자로 이동. 별도 inline 판정 금지 |
| Handler 실행 권한 | G `96`, `101–114`, `137–149`, `258–269` | C mailbox/serial gate(q:228), D mailbox/pump(q:243), J mailbox/serial gate, N serial queue | Execution gate 유지. 수신 역할 인계로 application 권한을 얻지 않음 |
| Permit·pressure | P `37–57`, `116–122`; H I1–I3 | 언어별 host application job queue(q:242–243) | 기존 host permit owner. Worker 가용성을 두 번째 capacity authority로 쓰지 않음 |
| Caller scheduler/context | qe BLOCKERS `204–205` | .NET caller context, Java *Async executor, Kotlin dispatcher | Application/언어 runtime 유지 |

**Core의 completion owner와 Framework ingress 실행 역할은 다른 사실이다.** BA `67–73`은 binding이 socket마다 public poller wait 구동 자원 하나에서 completion을 drain하고 background owner를 두지 않도록 정한다. CP `133–141`은 completion registration·직렬 drain 하나, `158–160`은 active wait에 대한 동시 조작 금지를 정한다. 이전 wait/drain이 반환된 뒤 다른 자원이 같은 poller의 다음 wait를 호출하는 구조는 기존 직렬화 계약으로 구성할 수 있다. 인계마다 새 poller·registration remove/add를 만들 이유는 없다.

### 2.2 A와 server ingress owner

| 언어·경로 | Client 응답 알림 자원 A | Server ingress owner·소유 계층 | 인계 판단 |
|---|---|---|---|
| C++ RouteMesh | Framework node 수신 thread의 public poller. Binding `bindings/cpp/src/Runtime/Eventing/poller.cpp:401–404`가 drain하고 같은 자원의 terminal observer가 registry를 완료. qe C 표의 `raw_binding_adapter.hpp:20–35`, `raw_mesh_node_owner.cpp:979–1050` 인용 | C `src/runtime/mesh/mesh_node_host_service.cpp:2567`의 수신 thread. q:226–229의 raw port claim→mailbox→worker 경로. Framework 소유 | 가능 후보. Callback 안이 아니라 wait·receive turn·socket/registry lock·lane-current scope 반환 뒤 외부 수신 경계에서 인계 |
| C++ ClientServer | qe:207의 `raw_client_server_owner.cpp:1455`→`raw_dealer_port.cpp:112–115`→native await/coroutine executor. Binding terminal 발생 자원과 Framework 알림 continuation 자원이 다를 수 있음 | C `src/runtime/channels/channel_host_service.cpp:600,614`의 Framework thread와 raw router/transport poller. 현재 `raw_dealer_port.cpp:124–129`는 shared poller owner만 wait함을 명시 | RouteMesh registry 수정만으로 안 됨. Native continuation 선행 게시까지 통합 필요. Owned-poller fallback도 같은 hop으로 단정하지 않음 |
| .NET RouteMesh direct | 물리 drain은 Framework receive loop. `bindings/dotnet/src/Zlink/Runtime/Eventing/Poller.cs:250,291`에서 Drain. Request TCS `CompletionOwner.cs:942`의 RCA 때문에 **Framework 알림 A는 ThreadPool continuation**(qe D direct 표) | D `Service/ZLinkManagedMeshNode.cs:436–441`의 LongRunning task, `:5283–5293` ReceiveLoop/Wait. Framework 소유 | I4 수신 역할은 후보. E5의 pool A에는 수신 역할이 없어 ② 직접 적용 불가 |
| .NET ClientServer direct | D `Channels/ZLinkClientServerClientRuntime.cs:289–304`의 binding Task 소비(qe). 물리 완료는 connection poller, 관찰은 RCA pool | Client: 같은 파일 `:1043,1056–1069,1462`. Server: `ZLinkChannelRuntimeManager.cs:112–125`→`ZLinkChannelReceiveLoop.cs:22,45,47–51`. Framework 소유 | E5 제한은 동일. Server의 `ZLinkChannelApplicationDispatchQueue.cs:10,49–50,107–124` 단일 Channel worker는 곧바로 인계 자원이 아님 |
| .NET table request | qe의 native pool→table dispatcher→reply RCA. 현재 D `Backend/DotNet/ZLinkCompletionDispatcher.cs:19`는 dedicated worker. m:220–224 최종안은 callback별 독립 pool work item에서 caller 실행 | Ingress는 위 node/Channel loop. Completion table과 별도 owner | m 최종안을 현재 worktree에 적용했다고 주장하지 않음. Table 알림 자원에는 수신 역할이 없을 수 있음. 독립 callback을 단일 drain으로 합치면 안 됨 |
| Java RouteMesh | J `runtime/binding/ZLinkJavaRawMeshNode.java:4555–4566`의 platform pump. Framework `ZLinkJavaSocketReceivePoller.java:95–100`가 wait하고 binding terminal의 non-Async whenComplete가 관찰(qe J 표) | 같은 pump의 `pumpTurn(:4570–4585)`, `drainIngressBatch(:4592–4602)`. Framework 소유. 경로 이름 runtime/binding은 binding package가 아님 | 소유 계층상 가능. 현재 single-thread executor에 다음 pump를 게시하면 같은 thread이므로 인계되지 않음 |
| Java ClientServer | J `runtime/channels/ZLinkChannelSocketRegistry.java:1351–1370`의 DEALER owner가 completion도 구동. `:1409–1422`, `ZLinkJavaDealerSocket.java:80–86`; `ZLinkChannelCallRuntime.java:112–125,137–160`이 operation registry에 연결 | J `runtime/channels/ZLinkChannelReceiveLoops.java:27–36,43–57,193–209`의 Framework cached-executor loop. `ownerThread(:179,196)` 수신 소유 표시 | 소유 계층상 가능. `ChannelSocketRegistry:1386–1391`의 thread 종료 시 socket close를 logical 역할 종료로 이동 필요 |
| Kotlin | Java native pump·registry·dispatcher 동일. K `ZLinkCoroutineTurnAwait.kt:17–25` resume은 coroutine context를 따름(qe) | Java ingress 재사용. K `ZLinkCoroutineSuspendHandlerInvoker.kt:24–29,47–49`, `ZLinkCoroutineHandlerOptions.kt:12–22` Default/사용자 dispatcher 유지 | Java 인계 후에도 coroutine 첫 instruction을 receiver에서 실행한다고 보장할 수 없음. Dispatcher 강제 치환 금지 |
| Node RouteMesh·ClientServer | N `backend/node/node-event-loop-poller.ts:28–29,48–53` readiness callback→같은 JS event loop의 wait(0)→Promise reaction(qe Node 절) | N `foundation/raw-service-mesh-runtime.ts:644,668,746` pump/claim; ClientServer는 `channels/channel-receive-loops.ts`와 `:30–39` serial queue, `backend/node/node-raw-binding-port.ts:143–148`의 같은 poller | 인계받을 다른 JS 실행 자원 없음. Worker Thread로 socket/runtime 상태 이동은 이번 안이 아님. Microtask 새 turn 유지 |

Completion connection을 Framework ingress poller로 받는 대안은 C++·Java·Node에 이미 존재한다. .NET도 물리 completion은 그렇게 받는다. .NET RCA는 poller 재배치만으로 없어지지 않는다. Core가 completion connection을 결정하는 부분을 Framework가 다시 구현하지 않는다(G `143–145`, H `27`).

## 3. 인계 구조와 자원 상한

### 3.1 성립 조건

다음은 구현 승인이나 현행 계약이 아니라 ②의 가능한 설계 경계다.

1. E3의 기존 완료 자리 또는 owner queue에 work를 넣는다. Terminal atomic take와 admission은 기존 소유자가 수행한다.
2. 완료 처리, public wait/drain, receive turn, lock·lane-current scope를 모두 반환한다. 처리 stack 안에서 인계 후 application을 실행하지 않는다.
3. **기존 자원 배치 소유자 하나**가 대기 자원의 독점 사용권과 수신 역할 이전을 함께 확정한다. Socket·poller·cursor·batch/permit 수명은 logical role에 남는다.
4. 성공하면 대기 자원은 다음 수신 회전, 현재 자원은 기존 queue/dispatcher의 gate 가용 work를 맡는다. Current message가 queue 선두인 무부하 경우에만 같은 자원에서 그 message를 시작한다.
5. 대기 자원이 없으면 현재 자원은 수신을 계속하고 work는 기존 queue에 남는다. 새 fallback queue·timer·retry 없음. 수락한 callback을 버리지 않는다.

Successor 실제 시작을 기다리는 ACK는 두지 않는다. ACK가 새로운 대기를 critical path에 만든다. 반대로 일반 executor의 게시 성공을 독점 예약으로 간주하지 않는다. 다른 worker가 application 일을 받아 막힐 수 있으면 수신 진행을 보장하지 못한다. 독점 예약은 기존 자원 owner의 worker 상태에서 표현해야 하며 callback별 generation/flag를 추가하지 않는다.

수신 역할마다 실행 가능한 자원 하나는 남아야 한다. G `33–35`의 진행 영역 분리를 유지하려면 queue 자리보다 강한 **실행 자원 확보**가 필요하다. Logical 역할 교환을 기존 진행 영역 분리와 동치로 인정할지는 BLOCKER다. Gate 권한은 별도이므로 receiver가 gate를 동기 대기해서도 안 된다.

### 3.2 재사용할 자원·부족 경로

| 언어 | 기존 후보·한계 | 상한과 fallback |
|---|---|---|
| C++ | Offload worker·completion worker. `offload_executor.cpp:22–36,77–78`의 bounded worker·idle/live count 재사용 후보. 현재 work 소비자는 poller 역할 양도 기능 없음. ASIO coroutine executor(`coroutine_executor.cpp:64,86`)도 idle physical worker 예약 API 없음 | 기존 ingress R+offload 상한 W+completion/coroutine 자원 수 안에서 교환. Callback별 새 thread 0, 상한 증가 0. 정적 `_threads` 소유와 별도 completion-worker 배치를 교체해야 함. 모두 busy이면 queue 유지·receiver 유지 |
| .NET | Mesh persistent task(D `ZLinkMeshDispatchPump.cs:95–100`)는 max(2,ProcessorCount)개. `RunAsync:272`의 semaphore waiter는 특정 physical thread를 예약하지 않음. ThreadPool 게시 성공도 예약 아님 | Task 수와 physical worker 수를 동일시하지 않음. I4 bounded 자원 교환 설계 먼저 필요. E5는 RCA 유지 시 native→pool 1회 잔존. ThreadPool 한도나 정책을 Framework가 복제하지 않음 |
| Java | Pump는 single platform executor. `ZLinkProcessExecutionLanes.java:22–24`는 virtual thread-per-task, `:32–39,70–89`는 SerialLane. 재사용 가능한 idle platform-worker 집합이 아님. ClientServer cached executor(`ChannelReceiveLoops:27–36`)도 successor별 생성 상한 없음 | 기존 수신·완료·application 자원 소유를 통합하는 bounded 구조 필요. Cached successor·virtual thread-per-message 추가 금지. Virtual thread·carrier·native wait 자원 수는 구별 |
| Kotlin | Java 자원만 인계 후보. Default/사용자 dispatcher의 physical idle worker 예약은 Framework 소유가 아님 | Context의 dispatch가 필요하면 기존 게시 잔존. 사용자 dispatcher 우회 금지 |
| Node | 다른 JS 실행 자원 없음. Event loop 하나 | 자원 1 유지, 역할 인계 없음. Suspension 사이 교대와 예산 끝 macrotask 양보(H `300–302`) 유지 |

**고정 실행 자원 상한과 임의 application 동기 대기의 진행을 일반적으로 함께 보장할 수는 없다.** W개 업무 자원이 모두 미실행 callback의 결과를 동기 대기하면 receiver가 terminal을 확정해도 callback 실행 자원이 없다. 무한 thread 생성·blocked stack 이동·application 재진입 실행은 이번 범위 밖이다. W≥2에서 두 callback 격리를 검증하는 것과 임의 `.Result` 의존 사슬을 보장하는 것은 다르다.

m 최종안은 독립 ThreadPool work item으로 Framework의 단일 drain 의존을 제거했다. Pool의 worker 증가·상한·기아 정책은 언어 runtime 소유다. ②로 이 구조를 대체하려면 동기 대기의 허용 범위와 자원 배치 계약을 먼저 정해야 한다.

## 4. 보존 계약 대조

| 항목 | 조항·근거 | ②의 보존 조건·충돌 |
|---|---|---|
| §11 1~4단계 | S `402–405`, H `98,112` | Terminal 확정→unlock→예약 dispatcher 자리 게시→새 turn 실행 유지. 3 삭제 금지. 같은 자원은 외부 회전으로 반환한 뒤 기존 work를 꺼냄. Admission·timer·cancel·close처럼 수신 역할 없는 완료는 기존 dispatcher |
| Lock·scope·stack 밖 실행 | S `391–398`, H `98`; J receive poller `95–100` | Socket lock만 풀어서는 부족. Registry lock, Java synchronized wrapper, lane-current scope, native/binding 처리 stack 모두 종료. Binding callback scheduler를 inline로 바꾸는 방식 제외 |
| Completion FIFO | C registry `163–167,203–206`; J dispatcher `49–56,108–115` | 현재 FIFO 소비. S §11은 서로 다른 operation의 process 전체 callback 반환 FIFO를 정하지 않음. Submission FIFO(H `106–107`)·owner FIFO와 구별. 기존 대기 work를 건너뛰어 최신 message 직접 실행 금지. 병렬 callback의 선택·시작·반환 순서는 다를 수 있음 |
| 재진입 관측 의존 | `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_operation_registry.cpp:231–248,252–262` | Nested cancellation callback은 outer callback 반환 전에 실행되지 않는 assertion(`:241`)이 있음. 새 turn은 thread-ID 차이(`:258`)로 관측. 독립 callback 병렬화·동일 자원 새 turn과 충돌 가능. 규범 인과 순서와 현재 단일 worker의 결과를 먼저 구별. 구현에 맞춘 assertion 변경 금지 |
| Exception 격리 | S `429–430`; C registry `208–215`; J dispatcher `96–103`; m `222` | 기존 work 실행 경계의 reporting/finally·예약 반환 재사용. Receiver catch-all로 callback 오류를 숨기지 않음. Callback 실패가 successor 생존을 결정하지 않음 |
| Shutdown drain | S `411–427`; C registry test `265–305`; m `222` | Queue 밖에서 실행하는 work도 callback 반환까지 기존 reservation에 포함. Queue empty만으로 종료 금지. Role 교환·close 같은 resource owner에서 직렬화. Queued+running accepted callback 모두 drain. 영원히 반환하지 않는 callback의 유한 정상 drain을 보장하지 않음 |
| Gate·Yield | G `96,101–114,137–149`; H `113–114,213–217` | 역할 교환은 physical 배치뿐. 일반 await held-turn/context 복원, Yield의 queue 뒤 gate 재획득 유지. Busy gate 동기 대기·role flag를 두 번째 gate로 사용 금지 |
| I3 control | H `180,200–205`; P `116–122` | Claim된 control은 I3에서 처리·permit 반환, 업무 queue에 넣지 않음. 현재 batch control 처리가 끝나기 전 업무 전환 금지. Successor 실행 가능해야 다음 회전 진행. DATA FIFO·PAUSED·permit으로 claim 전 지연되는 기존 제약까지 제거한다고 주장하지 않음 |
| 병렬성 | H **§4.3** `225–227` | 질문의 §4.2 병렬성은 현재 §4.3에 있음. 다른 gate들을 한 worker로 직렬화하면 위반. Java SerialLane·.NET Channel 단일 worker를 재사용만 하고 계약 충족을 선언할 수 없음 |
| Permit·pressure | P `116–122,150`; H `178–181,215` | Ordinary claim 전 permit, queue 동안 유지, handler 첫 instruction 직전 반환. 인계 때 먼저 반환 금지. Pre-receive terminal completion의 기존 우회 유지. Idle worker 수를 capacity로 투영하지 않음 |
| .NET task.Result/GetResult | m `202–212,220–224` | a continuation에서 b를 동기 대기하고 b reply를 driver가 허용한 공개 재현. 단일 drain은 5초 guard까지 실패, a release 전 b continuation 검사도 실패. 독립 work item은 통과. ②에서 receiver만 진행하고 b callback이 같은 업무 자원을 기다리면 같은 교착 |
| Context·Kotlin | qe `204–205`; K HandlerOptions `12–22` | Framework 중복과 caller 선택 게시를 구별. Context/dispatcher 강제 제거로 0 hop을 맞추지 않음 |
| Batch·관리 | H `179,191–198,217–223` | 첫 application record마다 인계하면 64건 batch·회전당 관리 한 번이 깨질 수 있음. 기존 receive 회전 종료에서 선택하며 record별 handoff·batch reset 금지 |

## 5. 효과 추정

### 5.1 남는 깨우기·전환 수

Ready target, pending reply에 이미 continuation이 등록된 기본 비동기 caller 기준이다. 아래 숫자는 **I4/E5 관련 physical 자원 이전**이다. 잠든 자원이면 wake, 이미 실행 중이면 queue handoff일 수 있다. Core I/O·수신 readiness wake·제출 lane 경합·caller 선택 scheduler·bench CV는 별도다. Physical 전환 0은 새 turn 0이 아니다.

| 언어·경로 | 현재 | ② 최선 조건 | 잔여·제약 |
|---|---:|---:|---|
| C++ RouteMesh async | I4 1+E5 1=2 | 0 | Idle 자원·queue 선두·gate·scope 반환 모두 필요. Busy fallback 2. Blocking bench caller의 wake는 별도 |
| C++ ClientServer | I4 1+native continuation executor 1=2(qe) | 0 후보 | Native 게시까지 통합 필요. RouteMesh registry만 고치면 완료 전환 잔존. Legacy offload 일반화 금지 |
| .NET direct OK 두 경로 | I4 1+binding RCA 1=2(native terminal 기준) | I4 성립해도 최소 1 | Framework 알림 진입 이후 추가 강제 E5 게시는 0(qe). Pool A에서 인계해 native RCA 1→0은 불가 |
| .NET table 현재 worktree | I4 1+native RCA/table dispatcher/reply RCA 최소 3=최소 4(qe) | 현재 구조만으론 확정 못함 | m 최종 revision은 I4 1+native RCA 1+독립 table post 1=최소 3. 거기서 I4 인계까지 되면 최소 2. 서로 다른 revision의 4→2를 인계 효과로 주장하지 않음 |
| Java non-Async 일반 caller | I4 1+dispatcher 1=2 | 0 후보 | Handler caller는 qe manageCurrent 추가 게시 1이 잔존. 이것은 인계와 별도 B 후보. Yield gate 재획득 제외 |
| Kotlin dispatch 필요 await | Java 2+coroutine 게시 1=3 | 최소 1 | Handler caller는 현재 4, 인계만으로 최선 2. 사용자 dispatcher/Async chain 추가 수는 고정 상한으로 단정 못함 |
| Node | OS thread wake 0, 추가 macrotask 0 | 0 | Promise reaction·gate queue·예산 끝 macrotask 유지. C++ µs로 환산 금지 |

②에서는 일반적으로 I4·E5의 successor 통지 **총 2개가 남는다.** 현재 message를 다른 잠든 자원에 맡기지 않아 시작 지연을 겹칠 수 있다는 뜻이다. Notify 호출 시간·scheduler 경쟁·cache 이동이 0이라는 뜻이 아니다. Request-serial의 다음 응답은 successor 준비에 의존하므로 steady-state 처리량에서 이 비용을 전부 지연 밖으로 제외할 수 없다.

### 5.2 기존 µs 값만 사용한 추정

식은 `F'≈F−I4 대기−E5 대기+인계/경합 비용`이다. 표는 마지막 항을 0으로 둔다. 구간 중앙값 합은 전체 RTT 중앙값 감소량과 같지 않을 수 있어 정밀 예측·엄밀한 상한이 아니다. 다른 run의 중앙값을 합치지 않았다.

| 모집단 | 사용 값(µs) | 제거 가정 | Binding/Framework 추정 |
|---|---|---|---:|
| 배경 가정 + q 구간 | B=110, 비율=.65→F=169.231. q:178 I4=19.288, E5=15.528 | 감소 34.816→F'=134.415 | **81.84%**. 실제 run 아님 |
| q clock-only 중앙값 | q:173 B=109.909, F=270.642; q:178 합 34.816 | F'=235.826 | **46.61%**, 현재 RTT 비율 약 40.61% |
| q 같은 실행 평균 | q:176 B=116.886, F=278.026; q:178 평균 합 36.177 | F'=241.849 | **48.33%**, 현재 약 42.04% |
| p await branch 계측 | p:138–146 B=115.531, F=265.073; I4=20.219, E5=15.760 | 감소 35.979→F'=229.094 | **50.43%**, 현재 약 43.59% |

배경 가정의 90% 목표는 F≤122.222µs, 필요한 감소량 47.009µs다. 34.816µs를 모두 제거해도 약 12.193µs가 추가로 필요하다. 두 wake를 각각 15–19µs로 잡아도 비율은 약 79.0–83.8%다. **②만으로 90%를 보장할 근거는 없다.** q:8의 비계측 cycle 343.74/155.44µs, p:128의 47.94%는 clock-only 값과 합산하지 않았다.

.NET·Java·Kotlin·Node의 해당 구간 µs 측정은 q·p에 없다. C++ 값을 다른 언어의 예측으로 쓰지 않는다. .NET direct에서 제거 가능한 I4 하나만 배경 가정과 C++ 값으로 계산하면 약 73.36%이나 이것도 .NET 측정·예측이 아니다.

### 5.3 연속 부하 위험

- Record별 교환은 기존 batch에 분산한 관리·wake 비용을 다시 낱개 비용으로 만든다. Receive 회전 종료 선택과 기존 owner drain을 유지해야 한다.
- Socket 진행은 계속 직렬이다. Poller/binding scratch·codec/DI cache가 자원 사이를 이동하면 locality 비용이 추가될 수 있다. Java virtual/application 자원을 native wait에 배치하는 비용도 미측정이다.
- Window에서는 기존 queue backlog가 있어 current message를 먼저 실행할 수 없다. Serial의 0 hop을 얻지 못할 수 있고 긴 callback은 업무 자원을 점유한다.
- Permit 0이면 successor도 ordinary receive 불가. Completion·관리·종료 진행 유지, worker 가용성으로 Core admission/PAUSED 재판정 금지.
- Send 즉시 admission·timer/cancel/shutdown 완료에는 인계할 수신 역할이 없을 수 있다. 기존 dispatcher 경로가 필요하며 별도 inline fast path를 추가하면 판정 수가 늘어난다.
- Wake 지연을 겹쳐도 CPU 비용은 남는다. 개선 후 serial만 보고 send·window·backpressure 성능을 선언할 수 없다. 이번 job에서는 측정하지 않았다.

## 6. 스펙 개정 후보 문안 — 보고서 안의 조건부 초안

아래는 감독자의 판단 자료이며 현행 계약·구현 승인이 아니다. 충돌·미정은 §8 BLOCKERS에 적었다. 한국어 기준 확정, 보호 경로 승인, 대응 영어 동치와 sol 읽기 전용 리뷰 후에만 구현할 수 있다.

### 6.1 S §11: 완료 공개·새 turn

S가 terminal 공개·자리 수명·격리·drain을 소유하고, 자원 배치는 H 한 곳을 참조한다.

> 완료 권한 확정, 잠금 해제, 예약한 dispatcher 자리 게시, 새 execution turn에서의 callback 실행 순서를 지킨다. 현재 완료 처리와 그 호출 stack, lane-current scope가 반환되기 전에는 application callback을 실행하지 않는다. 새 turn은 다른 thread에서 실행해야 한다는 뜻이 아니다. 실행 자원 배치는 Messaging hot path의 수신 역할 배치 규칙을 따른다.
>
> Completion dispatcher는 process가 공유하는 완료 공개 주체다. 전용 thread 또는 callback 전체를 직렬화하는 drain을 뜻하지 않는다. 자리는 callback 반환까지 유지하며 shutdown은 이미 수락한 queued·running callback을 모두 실행한 뒤 종료한다. 한 callback의 exception은 다른 callback의 실행을 막지 않는다.

S `402–405`의 1~4단계는 삭제하지 않는다. `425–426`의 공유 lane을 physical 단일 thread로 해석하는 여지를 제거하는 후보다. 임의 동기 대기 진행을 새로 보장하지 않으며 m의 독립 work-item 의미를 축소하지 않는다.

### 6.2 H §3 E5·전환 정의

> E5는 완료 알림 자리에서 terminal 권한을 원자적으로 확정하고 E3에서 예약한 dispatcher 자리에 결과를 전달한다. 새 turn과 자리 수명은 Submit과 완료 §11을 따른다. Callback 실행 자원은 이 문서의 수신 역할 배치가 결정한다. Dispatcher 앞의 추가 mailbox·dispatch thread는 두지 않는다.
>
> Execution turn의 경계와 physical 실행 자원 전환을 구별한다. Physical 전환은 해당 message 또는 완료의 실행이 다른 자원으로 이전하는 지점이다. 같은 자원에서 현재 처리가 반환된 뒤 실행하는 새 turn은 physical 전환 0회다. 완료 구간의 Framework 추가 physical 전환은 인계 성공 때 0회, 기존 dispatcher 전달 때 1회다. Yield의 gate 재획득과 application이 명시적으로 선택한 scheduler/context 게시를 Framework 추가 전환과 구별한다.

H `100–106`의 dispatcher turn 하나=자원 전환 하나라는 등치를 제거하는 후보다. E5에 idle 검사·인계 조건을 재서술하지 않는다. Application context 제외 범위는 qe의 D를 해소하는 계약 개정이므로 아직 확정 아님. .NET의 binding RCA는 Framework 추가 전환 제외 여부와 관계없이 실제 RTT의 물리 게시로 따로 센다.

### 6.3 H §4.1 I4·§4.2 관리·§4.3 병렬성

역할 배치 규범은 H §4.1 한 절에만 둔다.

> Ingress owner는 readiness 대기, completion 진행, ordinary receive·claim과 회전별 관리 작업을 담당하는 logical 역할이다. Socket·poller의 수명과 receive cursor는 그 역할에 속한다. 실행 자원 배치 소유자는 수신 회전이 반환된 경계에서 다음 수신 회전을 실행할 가용 자원을 확보하고 역할을 이전한 경우에만 이전 자원에서 준비된 업무를 실행한다. 이전할 자원이 없으면 현재 자원이 수신 역할을 유지하며 업무는 기존 owner queue 또는 dispatcher에서 실행한다. Socket마다 동시 receive/completion 진행 자원은 하나이고 application 대기로 수신 진행 영역을 점유하지 않는다. Work마다 자원을 만들지 않고 기존 자원 상한을 늘리지 않는다.

I4 후보:

> I4는 header 분류, 원자 admission, owner queue 이동과 ready 등록을 수행한다. 업무 실행 자원 선택은 수신 역할 배치가 소유한다. 준비된 work는 기존 queue와 execution gate 순서로 실행하며, 수신 자원이 새 업무 turn을 맡아도 queue를 우회하지 않는다.

§4.2의 관리 회전당 한 번, 64건 batch/cursor·deadline 규칙은 유지한다. 역할 교환 때 batch reset·관리 중복 실행 규칙을 추가하지 않는다. §4.3 `209`, W1~W5는 worker를 업무 실행 역할로 연결하고 `225–227`의 다른 gate 병렬성은 유지한다. Gate 규칙은 G를 참조하며 H에 복제하지 않는다.

G `33–35`의 진행 영역 분리와 logical role 분리가 동치인지 먼저 판단해야 한다. 동치가 아니면 H만 고쳐 ②를 도입할 수 없으며 G를 약화해서는 안 된다.

### 6.4 H §6 언어 표

| 언어 | 후보 표현 |
|---|---|
| C++ | Framework ingress 역할을 구동하는 bounded 기존 자원 / 기존 executor 업무 자원 / process 공유 완료 공개 주체의 업무 자원. 통합 검증 후 physical 전용 thread 고정 표현 제거 |
| .NET | Framework receive loop / application worker task·ThreadPool / binding Task RCA 및 독립 completion work item. E5 wake 0이라고 쓰지 않음 |
| Java | 현재 platform pump(`RawMeshNode:4565`)로 설명 정정 후보. Virtual thread 고정값을 규범으로 만들지 않음. 자원 종류와 역할 교환 성공 조건 구별 |
| Kotlin | Java ingress·completion owner 공유, continuation 자원은 선택한 coroutine dispatcher. Java의 0 hop을 그대로 적용하지 않음 |
| Node | Event loop / 같은 event loop microtask / Promise reaction. 다른 수신 역할 자원 없음 |

표는 자원 종류만 적고 배치 판정을 반복하지 않는다. 표를 통일하는 것으로 서로 다른 자원 가용성의 동작 parity를 선언하지 않는다.

### 6.5 L 용어집

L `992–1001`의 Completion dispatcher 후보 정의:

> 확정된 Framework operation의 완료 callback을 새 execution turn에서 공개하고 callback 반환까지 예약 자리를 관리하는 process 공유 주체다. Pending registry의 terminal 권한 판정 및 Core/binding의 native completion drain과 구별한다. 실행 자원 종류와 배치는 Messaging hot path가 정한다.

H `50–53`의 ingress owner/application worker는 현재 장 내부 설명용이다. 정식 용어로 승격하면 **정의는 L 한 곳**, H는 링크로 교체한다. Ingress owner는 socket 수신 진행의 logical 역할, application worker는 gate에 따라 준비된 업무를 실행하는 자원으로 정의한다. 인계 조건·fallback·상한은 L에 반복하지 않는다. 인계 generation/flag는 새 용어로 만들지 않는다.

### 6.6 Binding·Core 필요 변경

**Physical 수신 역할 교환만으로는 Core spec 개정이 필요하지 않다.** CP `133–141,158–160`의 한 registration·직렬 wait/drain을 유지한다. Completion socket routing·HWM·physical dedup은 변경하지 않는다.

BA §4 `67–73`이 thread identity 영구 고정을 뜻하지 않음을 명확히 해야 한다면 같은 절을 아래로 교체한다. Framework 예외를 추가하지 않는다.

> 고수준 binding completion은 public poller를 호출자가 구동할 때 진행하며 background drain owner를 두지 않는다. Socket마다 completion poller registration과 직렬 drain 경로는 하나다. 현재 wait/drain이 반환된 뒤 같은 poller의 다음 wait를 다른 호출 실행 자원에서 구동할 수 있으며 이전 자원과 동시 진행하지 않는다. Physical 진행·registration·직렬화는 Core polling 계약을 따른다.

Java binding `bindings/doc/spec/java/README.ko.md:173–179`의 native wait thread/application executor 분리가 physical thread 영구 분리를 뜻한다면 충돌한다. 교체 후보:

> Runtime은 public poller로 readiness를 기다린 뒤 DONT_WAIT receive를 수행한다. Application handler는 native wait/receive 처리가 반환된 application execution turn에서 실행한다. Framework의 실행 자원 배치는 Framework 계약이 소유한다.

Handler 배치의 binding 문서 규칙을 삭제하고 Framework를 참조하므로 binding만의 thread 규칙을 두지 않는다.

**.NET E5 0 hop에는 위 문안만으로 부족하다.** RCA를 제거하면 `CompletionOwner.cs:1307–1316`의 Locked setter에서 application continuation이 실행될 수 있다. 물리 completion 결과를 lock·native 처리 밖으로 공개하는 공통 terminal 계약과 public 표면이 필요하다. Public poller의 progress 자체는 개별 Framework 예약 callback을 receiver 반환 경계에 전달하는 계약이 아니다. Reflection·private entry·raw completion ID 접근·pending Task 전수 scan은 제외한다.

BA §4·§5에 필요한 동작 수준 초안은 “terminal 권한·payload 수명 처리와 public continuation 공개를 분리하고 공개는 내부 lock·completion 처리 scope 종료 뒤 수행한다”이다. Exactly-once·취소·late payload·submit 합류를 보존해야 한다. Public signature·예약 수명·기존 Task continuation 정책과 공존 방식은 미정이므로 **BLOCKER D이며 충분한 API 초안이 아니다.** Core에 Framework callback scheduler를 추가하거나 binding 전용 pool을 만들어 해결하지 않는다. 이 미정 부분의 구현은 중단한다.

### 6.7 판정 위치 수

같은 실행 배치를 독립해서 내리는 코드 소유 지점을 센다. Registry terminal·gate·permit은 다른 결정이므로 감소 수에 합산하지 않는다.

| 범위 | 현재→② 후보 | 감소 조건 |
|---|---:|---|
| C++ RouteMesh 자원 배치 | 3→1 | 수신 `mesh_node_host_service:2567`, ready offload `:2558`, completion post `operation_registry:157`을 기존 자원 owner 하나로 **교체**. 기존 3+새 인계 분기 1=4면 반려 |
| Java RouteMesh 자원 배치 | 3→1 | `RawMeshNode:startPump`, `ProcessExecutionLanes:schedule`, `ServiceCompletionDispatcher:post`의 배치 권한 이동. Gate·permit 그대로 |
| .NET direct I4/E5 | 2→2, 현 계약 유지 | Framework handler 배치와 binding RCA는 계층이 다름. Framework에서 한 곳으로 합쳐 감소했다고 주장 금지 |
| Kotlin | Java 3+coroutine dispatcher 1=4→Framework 1+context 1=2 후보 | Application dispatcher 유지. 강제 치환으로 4→1 금지 |
| Node | 1→1 | Event loop owner 유지, 새 인계 판정 없음 |
| m 최종 .NET table | Post 1→1 유지 | m:224의 FIFO drain 예약·진행 **3→0**은 그 별도 수정 결과이며 ② 효과가 아님 |

위 숫자는 통합 설계 목표다. 현재 구현 가능성을 증명한 숫자가 아니다. ClientServer까지 실제 통합하기 전 전체 process 판정 수를 더해 확정하지 않는다. 특히 .NET 포함 전체 감소 수는 미확정이다.

문서 자원 배치의 규범 위치 목표는 H/S/Java binding의 physical 자원 지정 3→**H 1**이다. S는 새 turn·수명, G는 gate·진행 영역, BA/CP는 native 진행을 유지한다. 정의 승격은 H 1→L 1 이동이며 두 곳에 반복하지 않는다. **실제 repository 판정 위치 변화 0, 추가·삭제 state/flag/queue/thread/lock/timer/retry 0**이다.

## 7. 대안 비교와 추천

| 안 | 배치 판정 수 | 효과 | 위험·성립 범위 | 판단 |
|---|---|---|---|---|
| ② 인계 | C++/Java 3→1 통합 시. 기존 구조 옆 분기는 3→4 | 가용 자원·queue·gate 조건에서 I4/E5 2→0 후보. 역할 통지 wake는 남음 | .NET RCA, Kotlin context, idle 자원 예약 부재, bounded 동기 대기, 재진입 관측, G 분리 의미 미정. 네 언어 공통 구현은 아직 성립하지 않음 | 후속 후보(D), 현재 확정 불가 |
| 기각한 직접 실행 | Dispatcher+idle inline 검사로 증가. 삭제해 1처럼 보여도 수신 진행이 callback에 종속 | 짧은 callback에서 1 wake 절감 후보 | 긴 callback이 수신/control/관리 점유. Unlock만으로 new turn 충족 안 됨. 회계·재진입 우회 | 기각(C) |
| 현재 dispatcher 유지 | 현재 위치 유지. m의 callback 독립성 보존 | ② 효과 0, 기존 전달 비용 유지 | 성능 미달·표면별 차이 남음. 소유 계층·Task/Kotlin context 임의 변경 없음 | **이번 판단의 추천** |

승인된 ② 방향을 구현 가능한 범위로 좁히는 근거는 확보했다. 모든 경로에서 성립한다는 근거는 확보하지 못했다. 감독자는 D를 확정하고 실제 기존 자원 owner를 통합하는 변경에서 판정 수 감소를 검토해야 한다.

## 8. BLOCKERS

| 분류 | file:line | 미정·충돌과 중단 범위 |
|---|---|---|
| D: turn/physical 전환 | S `395–405`, H `98,100–106,181–187`, L `992–1001` | 현행은 dispatcher turn을 필수 전환으로 셈. 동일 receiver 반환 뒤 업무 turn 허용 개정 전 게시 삭제 중단 |
| D: 진행 영역 | G `26,33–35`, H `177–181,225–227` | Bounded 역할 교환과 진행 영역 분리의 동치, 실제 receiver 확보 미정. 단순 게시를 인계로 간주 금지 |
| D: 상한·동기 대기 | m `202–224`; D `ZLinkMeshDispatchPump:95–100`; J `ZLinkProcessExecutionLanes:22–24`; C `offload_executor:77–78` | Task/virtual/OS 자원이 다름. 전체 W가 Result 대기할 때 보장 불가. 무한 thread·단일 drain 보상 금지 |
| D: .NET E5 | binding `CompletionOwner.cs:942,1307–1316`; qe:202 | Pool A에는 수신 역할 없음. RCA 삭제 시 lock 안 continuation 위험. 안전 public terminal 설계 전 변경 중단 |
| D: Kotlin/context | qe `204–205`; K HandlerOptions `12–22`, TurnAwait `17–25` | Dispatcher 보장·H 계수 범위 미정. Unconfined/context 덮어쓰기 중단 |
| D: callback 순서 관측 | C++ `test_cpp_framework_operation_registry.cpp:231–262`; C registry `203–209`; J dispatcher `108–115` | Outer 반환 전 nested callback 금지·thread-ID turn 관측 존재. 병렬/같은-thread turn 보존 조건 확정 전 assertion 변경 중단 |
| D: binding wait 경계 | Java binding README `173–179`; BA `67–73`; CP `158–160` | Physical thread 고정 여부 판단 필요. 언어 전용 예외로 해결 금지 |
| B 후보: 기존 추가 게시 | qe `206–208` | Java held-turn·.NET managed terminal lane·C++ 표면별 owner. 선행 진단 인용만, 변경 전 감독 A/B 판단 필요 |
| D/설명 차이: Java 표 | H `308`; J RawMeshNode `4565` | Virtual 표기와 platform 구현 차이. 보호 스펙 설명은 감독자 소유. 이 차이만으로 runtime 결함 단정 안 함 |

## 9. 후속 구현 범위와 수정 전 실패 테스트

추천안 자체는 runtime 변경 없음이다. 아래는 감독자가 D를 확정하고 ②를 후속 채택할 때의 후보 목록이다. 새 helper·queue를 추가하는 작업이 아니라 기존 배치 소유를 이동·삭제하는 작업이다.

### 9.1 file:function과 삭제·이동 대상

| 언어·범위 | file:function 후보 | 삭제·이동 및 보존 |
|---|---|---|
| C++ I4 | C `src/runtime/mesh/mesh_node_host_service.cpp:mesh_node_host_service_t::start`(수신 lambda `2567`, ready 게시 `2558`), `drain_application_owner`; `src/runtime/stateful/public_host_runtime.cpp:wait_for_dispatch_activity`와 receive-turn 경계; `src/runtime/mesh/service_mailbox.cpp`의 admission/ready/drain | 정적 `_threads` 수신 배치를 기존 executor 자원 owner에 통합. Mailbox queue·ready·gate·permit 유지. `offload_executor.cpp` 기존 `_idle_workers`·`_live_workers`·mutex·queue 재사용 가능성 확인. 새 handoff queue/flag/lock 금지 |
| C++ E5 | C `src/runtime/foundation/operation_registry.cpp:operation_completion_dispatcher_t::post_chain/run`; `src/runtime/mesh/raw_mesh_node_owner.cpp` terminal observer; `src/runtime/backend/raw_binding_adapter.hpp:observe_request_completion` | E3 intrusive node·atomic take·drain reservation 유지. 실행 배치 통합 뒤 전용 `_worker`·ready CV wait 수명 제거 후보. Queue를 무조건 삭제하면 busy fallback·shutdown 회계 유실 |
| C++ ClientServer | C `src/runtime/channels/channel_host_service.cpp:start` receiver 배치; `src/runtime/client_server/raw_client_server_owner.cpp` request 완료; `src/runtime/backend/raw_dealer_port.cpp:request`; `src/runtime/dispatch/coroutine_executor.cpp:post_native_continuation` | Native 선행 executor 게시 선택까지 공통 배치 owner로 이동. 일반 coroutine scheduler 전체 삭제 금지. Poller 추가 금지 |
| .NET I4 | D `Service/ZLinkManagedMeshNode.cs:ReceiveLoop`; `Backend/DotNet/ZLinkMeshDispatchPump.cs:EnsureStartedOnLane/OnReady/RunAsync/DrainClaimAsync`; `Service/ZLinkMeshOwnedMailbox.cs` admission/drain; ClientServer `Channels/ZLinkChannelReceiveLoop.cs:RunClientServerLoopAsync`, `ZLinkChannelApplicationDispatchQueue.cs:RunAsync` | `_receiveLoop`·worker task·ready signal의 실행 선택 통합 가능 시 변경. `_signal`·`_pendingReadyDomains`·Channel `_worker`를 무조건 삭제하지 않음. 역할 소유·physical 자원 상한 설계 전 보류 |
| .NET E5 | binding `CompletionOwner.cs:RequestCompletionEntry/SetResultLocked`; public terminal/poller 계약; D `Backend/DotNet/ZLinkMeshCompletionTable.cs` pending execute/예약 회수와 `ZLinkCompletionDispatcher.cs:Post` | Public 계약 필요. RCA 단순 삭제 금지. m 최종 독립 work item·callback-return 회계 유지. m이 삭제한 `_gate`/`_head`/`_tail`/`Next`·process drain 재생성 금지 |
| Java RouteMesh | J `runtime/binding/ZLinkJavaRawMeshNode.java:startPump/pumpTurn/drainIngressBatch`; `ZLinkProcessExecutionLanes.java:applicationLane/SerialLane.schedule/drain`; `runtime/internal/service/ZLinkServiceMailboxScheduler.java:admit/drain`; `ZLinkServiceCompletionDispatcher.java:post/postChain/run/take` | Single pump·completion worker·SerialLane 배치 권한 통합. Mailbox·pending node·registration 회계 유지. Physical pump 반환을 logical 수신 종료로 사용하지 않음 |
| Java ClientServer·held turn | J `runtime/channels/ZLinkChannelSocketRegistry.java:startClientServerControlReceive/runClientServerControlReceive`; `ZLinkChannelReceiveLoops.java:ReceiveLoop.runTurn/receiveBatch`; `runtime/internal/service/ZLinkServiceOperationRegistry.java` terminal dispatch; `execution/ZLinkSerialExecutionQueue.java:manageCurrent`(qe B 후보) | Cached successor 증가 금지. Thread 반환 시 socket close를 role 종료로 이동. Held-turn 추가 executor 삭제는 별도 A/B 승인 후, gate·claim·context 복원 하나 유지 |
| Kotlin | K `ZLinkCoroutineTurnAwait.kt`, `ZLinkCoroutineSuspendHandlerInvoker.kt`의 Java 완료 연결·context 복원 검토 | Dispatcher/context 삭제 없음. Java 부분 감소만 인정. Receiver 강제 치환 제외 |
| Node | N `backend/node/node-event-loop-poller.ts:onReady`, `backend/mesh-completion-table.ts` terminal, `foundation/raw-service-mesh-runtime.ts:pumpBatch`, Channel receive-loop serial dispatch | 기본 runtime 변경 없음. Microtask·예산 끝 macrotask 보존 contract/계수 대조. OS thread 추가 0 |

.NET의 미정 public API를 Framework internal adapter로 먼저 구현하지 않는다. Thin wrapper 원칙과 공통 terminal 계약을 승인한 뒤 exact signature를 정해야 한다.

### 9.2 수정 전 실패 조건과 양방향 검사

이 job에서는 테스트를 작성·실행하지 않았다. 아래는 후속 before/after 판정 조건이다. Thread-ID만 비교하거나 시간 threshold를 완화하는 검사로 대체하지 않는다.

| 검사 | Before 또는 나쁜 구현의 실패 조건 | After의 관찰 조건 |
|---|---|---|
| Serial I4/E5 전환 | Sleeping worker·ready target의 public typed request에서 현재-message 자원 이전 발생 | Idle 자원·queue 선두·gate 가용 조건에서 receive→handler, completion→caller physical 이전 0. Producer stack/lock/lane scope 종료 기록 true. Handoff wake와 업무 실행 wake를 구분 |
| 독점 수신 인계 | Old와 successor가 같은 poller wait/drain/recv 동시 진입 또는 Busy 오류 은폐 | Role별 동시 native 진행 최대 1, poller 등록 1. Old native scope 반환 뒤 successor 진입. Generation/flag로 수신 message 필터링 금지 |
| Control·관리 | a handler/continuation을 release event로 막고 ACK/probe/control 주입 시 release 전 I3 처리 없음 | Successor I3·관리·종료 진행이 a release 전에 관찰. Ordinary control은 permit 제공 조건으로 분리, pre-receive terminal completion은 permit=0에서도 확인 |
| Idle 자원 0 | 모든 업무 자원 busy인데 receiver가 application으로 이동해 completion/control 중단 또는 자원 추가 | Work는 같은 기존 queue 유지, receiver 진행, 자원 high-water가 기존 상한 이하, accepted callback 유실 0 |
| .NET 동기 대기·HOL | m `202–206`처럼 미완료 a await 등록 후 reply 허용, a continuation에서 b Result/GetResult 대기 | 다른 Actor/gate의 b reply 허용 뒤 a release/guard 전에 b 진행. Callback 두 개 격리와 W 전체 saturation을 별도로 관찰. W 전체 무조건 진행은 BLOCKER |
| Gate·Yield | Role 이전만으로 gate 없이 실행, 다른 gate도 단일 drain 대기, Yield가 FIFO 재획득 우회 | Same-gate overlap 0, 다른 gate b가 blocked-a release 전 진행. Held-turn 권한/context 유지, Yield는 기존 queue 재획득 뒤만 재개 |
| New turn·재진입 | Binding callback/native wait stack 안 public callback 시작 또는 lock 소유 중 재진입 | Same-resource producer 처리 반환이 callback 시작보다 먼저 기록. C++ `:231–262` 인과 기대를 spec 확정 후 검증. 기존 assertion 선삭제 금지 |
| Queue 순서·exception | 같은 owner의 이전 work를 건너뜀, callback failure가 뒤 callback을 막음 | Owner FIFO, terminal exactly-once, 오류 reporting·finally, 뒤 callback 실행. Global 반환 FIFO와 queue 선택 순서 구별 |
| Shutdown | Callback 실행 중인데 queue empty만 보고 종료 | Release 전 정상 drain 완료 없음. Release 후 accepted callback 전부 실행·예약 0. 인계 자원이 socket을 조기 close하지 않음 |
| Kotlin·Node | 강제 Unconfined/context override 또는 Node setImmediate 추가로 수치만 맞춤 | 선택 dispatcher에서 continuation. Node microtask 새 turn·I/O 양보 유지, 추가 OS receiver/worker 0 |
| 연속 부하 | Serial만 최적화하고 record별 인계·새 task 생성 | Send·window·backpressure의 회전별 batch/관리 횟수, task·할당·총 wake·high-water와 throughput/main 대비 새 실패 확인 |

Positive 결과뿐 아니라 stack 안 직접 callback, 미확보 successor 게시, 중복 native 진입, blocked single drain을 넣어 검사 자체가 실패하는지 확인한다. Cleanup guard는 테스트 종료용이며 runtime timeout·budget을 늘리지 않는다. Trace off hot path에 ungated 계측 비용을 남기지 않는다.

### 9.3 진단의 한계

- 현재 physical dispatcher 게시가 다른 thread에서 실행된다는 사실만으로 producer thread의 모든 stack 반환과 callback 시작 사이 wall-clock happens-before를 증명할 수는 없다. C `operation_registry.cpp:169,190–209`는 notify/별도 소비 구조이며 별도 return ACK가 없다. S의 새 turn 요구를 “다른 처리 stack에서 독립 실행”으로 보는 것과 “producer 전체 반환 뒤에만 다른 thread도 시작”으로 보는 것은 다르다. 이번 ②는 같은 자원의 외부 회전으로 반환하는 경계를 요구하므로 이 차이를 숨기지 않는다. 더 강한 global 반환 순서를 새 ACK로 보상하지 않고 감독자 해석을 먼저 확정해야 한다.
- Node는 인계로 줄일 wake가 없다. 동기적으로 오래 실행하거나 막는 JS callback이 event loop 진행을 늦추는 기존 제약도 이번 안으로 해결되지 않는다.
- 정적 조사로 successor 시작 지연·idle 분포·wake overlap·cache locality를 검증하지 않았다. C++·Java 가능성은 계층/계약상 가능성이지 최종 구현 검증 결과가 아니다.

## 10. 완료 상태

읽기 전용 소스·계약·선행 보고서 대조를 완료했다. Runtime 변경·수정 전 재현·인계 성능 측정·build/test/format 실행은 하지 않았다. 남은 사항은 §8 D/B 후보와 §9.3 해석 경계이며 90%와 네 언어 전체 판정 위치 감소는 미확정이다. 실제 저장소 변경·commit은 없다.

소유 계층: Core/binding은 physical completion·직렬 drain·HWM·payload 수명, Framework는 logical terminal·수신 역할·업무 배치·gate·permit, application/언어 runtime은 명시적 continuation context를 소유한다.
spec 조항: S §10·§11, H §3 E5·전환 정의·§4.1 I3/I4·§4.2 관리·§4.3 병렬성·§6, G §1·§3·§4, P §1·§3, L Completion dispatcher, BA §4·§5, CP §4·§5, Java binding Native Wait Boundary.
교차언어 대조: C++·Java는 Framework poller 역할 인계 후보, .NET E5는 RCA pool 알림으로 직접 적용 불가, Kotlin dispatcher 보장 유지, Node JS event loop 인계 자원 없음. RouteMesh/ClientServer의 별도 배치를 구분했다.
변경 분류: **D(스펙·실행 자원 계약 미정)** 읽기 전용 진단, 구현 없음. 기존 중복 게시 B 후보는 qe 인용, 역할 인계 없는 직접 실행은 C로 기각. 스펙 확정 뒤 적응 구현은 A로 다시 판정해야 한다.

