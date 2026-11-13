=== zlink-systems 2026-10-26T05:54:09Z
C++ 2차 감사(송신 경로·payload 복사, 취소·종료, relocation, state 소유) 결과로 위반 6건을 더합니다. 보고: `D:/t/jobs/fx/cx/acpp/report.md` 「2차 감사」 절.

- [ ] V11 수동 ClientServer native client가 요청·송신마다 endpoint provider를 호출해 endpoint 문자열 set을 다시 만들고 전체를 대조(08 §3 E2, `08:127-139`). `connection_version`이 같아도 생략하지 않음
- [ ] V12 자동 ClientServer outbound·reply가 native body→vector→native body로 전체 복사를 반복(08 §3 E1, 05 §2)
- [ ] V13 legacy(non-mesh) RouteMesh request가 별도 executor worker에서 연결 준비·binding request·`pending.result()` 대기(08 §3 E3/E4). `route_channel_runtime.cpp:223-269`
- [ ] V14 host가 `close_all_user_spots(deadline_at)`에 넘긴 shutdown deadline이 `close_spot`·`on_closing`까지 이어지지 않음(`time_point::max()`·빈 stop token)(03 §5 `03:130-146`, C++ Spot closing context)
- [ ] V15 session actor binding의 `actor_tokens`·`ready_actors`·`actor_streams`·`next_actor_slot`(C2 상태)을 state lane이 아니라 mutex가 소유(06 §4)
- [ ] V16 Fanout location runtime의 subscriber·snapshot sequence·observer 결정(C2)을 `_gate` mutex가 소유(06 §4)

남은 C++ 범위: connector core·engine adapter, 판정 보류 8건(보고서에 근거).

🤖 Generated with [Claude Code](https://claude.com/claude-code)

=== zlink-systems 2026-10-26T05:57:31Z
Java 감사를 sol로 교차 확인했습니다(luna 판정은 위반 2건이었고, sol은 위반 7건으로 정정). 보고: `D:/t/jobs/fx/cx/ajava/report.md`(sol), luna 판정은 `report-luna.md`.

- [ ] F1 송신 payload 전체 복사(05 §2·§5, 08 E1) — 앞 목록과 같음
- [ ] F2 수신 빈 검사에서 전체 복사(05 §2) — 앞 목록과 같음
- [ ] F3 remote Actor request가 binding reply를 future 여러 단계(port future → operation registry → `thenCompose` → typed 결과)로 다시 연결(01 §9 최종 typed 수신자를 제출 전 준비). `ZLinkJavaRawServicePort.java:282-320`, `ZLinkActorClientRuntime.java:298-342`
- [ ] F4 공개 request stage의 cancellation이 원 operation까지 전달되지 않음(03 §1·§3). `ZLinkActorClientRuntime.java:163-194,331-342,503-545`
- [ ] F5 request 등록(monitor)과 binding 제출(다른 port lane)이 갈라져 E3/E4를 같은 owner turn에서 확정하지 않음, registry C2 상태를 monitor가 소유(08 E3/E4, 06 §4). `ZLinkServiceOperationRegistry.java:31-51,174-201`
- [ ] F6 handler·filter 생성 절차를 startup에 확정하지 않고 dispatch마다 constructor 후보를 다시 고름(08 W1~W3 `08:241-247`). `ZLinkSpringHandlerFactory.java:83-100,285-309`
- [ ] F7 Actor send가 이미 확정된 admission 결과를 `adaptOneWay`로 두 번 감싸 새 completion owner를 만듦(01 §4). `ZLinkActorClientRuntime.java:220-254`

정정: 앞에서 스펙 빈칸으로 적은 G1(STREAM packet 실행 자원)은 빈칸이 아닙니다. 08 `:44-48`이 STREAM packet 경로를 범위 밖으로 두고, 07 `:102-105`가 session queue 책임을 따로 정합니다. 이 항목은 목록에서 뺍니다.

남은 Java 범위: 판정 보류 87행(보고서 근거).

🤖 Generated with [Claude Code](https://claude.com/claude-code)

=== zlink-systems 2026-10-26T12:28:33Z
C++ 3차 감사(connector core·engine adapter, 보류 재판정) 결과로 위반 5건을 더합니다. 보고: `D:/t/jobs/fx/cx/acpp/report.md` 「3차 감사」 절.

- [ ] V17 connector Immediate packet이 직접 실행되지 않고 항상 delivery strand에 게시됨(stream connector 스펙 `SC:589-592`). `zlink_stream_calls`·connector runtime `CC:1001-1056`
- [ ] V18 binding reply를 raw completion source/task로 감싼 뒤 observer로 typed 결과를 만드는 중간 비동기 결과(01 §9 `01:330-332` 최종 typed 수신자). `mesh_node_runtime.cpp:4098-4115`, `call.hpp:205-234`
- [ ] V19 Unreal adapter가 core callback을 종류별 queue로 나눠 state→reply→packet→request 고정 순서로 실행해 도착 순서를 바꿈(connector Manual callback 순서, C++ engine adapter 계약). `ZLinkStreamConnector.cpp:216-413`
- [ ] V20 C++ 공개 `dispatch()`가 callback 하나가 아니라 cutoff까지 queue 전체를 실행(C++ exact interface "한 callback"). connector runtime `CC:1657-1704`
- [ ] V21 legacy Spot request가 연결 대기·peer ready 대기·binding에 원래 timeout을 각각 그대로 넘겨 하나의 timeout budget을 지키지 않음(01 `:356-363`). `route_channel_runtime.cpp:349-394,468-521`

#2757(unknown Actor slot `frame_decode_failed`): 오류 분기 자체는 스펙대로입니다. 원인 후보는 server의 sink 조기 공개와 unbound 전후 제출 순서이며 확정되지 않았습니다(보고서).

C++ 위반 합계 21건(V01~V21). 남은 C++ 범위는 판정 보류 항목뿐입니다.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

=== zlink-systems 2026-10-26T13:37:50Z
F1·F2 수정: #2770(kairos 성능 리뷰 통과, 큐). 남은 보완: 비-owned serializer의 deserialize 복사 1회(`ZLinkMessagePayloads.java:43-45`)는 #2770 머지 뒤 같은 이슈에서 이어서 봅니다.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
=== zlink-systems 2026-10-26T14:50:27Z
- [ ] F8 (Java, kairos c-b10 교차언어 대조) `ZLinkJavaRawMeshNode.java:6967-6980 dispatchActor`가 authority 조회의 예상 밖 실패·취소를 `SOURCE_OWNER_UNAVAILABLE`로 정산해 오류 분류를 잃음(오류 모델 `07-framework-error-model.ko.md:39,99-111`, 취소 03 `:61-72`). C++ 같은 결함(`raw_stateful_dispatch.cpp:707`)은 kairos가 framework-cpp/1639-b5에서 고침. 남은 record 처리 규칙은 스펙 빈칸이라 04 §?에 감독자가 규칙을 넣는 중(sol 리뷰).

🤖 Generated with [Claude Code](https://claude.com/claude-code)
=== zlink-systems 2026-10-26T14:54:26Z
스펙 #2774(확인 단계의 예상 밖 오류는 그 message 하나의 실패)에 맞춰 고칠 항목:
- [ ] C++ `raw_stateful_dispatch.cpp:707 ingest_claimed` catch-all(kairos 수정 중, framework-cpp/1639-b5)과 `public_host_runtime.cpp:8245-8269 fail_application_claim`의 claim 전체 반환
- [ ] .NET 같은 경우의 claim 중단
- [ ] Java F8

🤖 Generated with [Claude Code](https://claude.com/claude-code)
=== zlink-systems 2026-10-26T15:05:07Z
분담(실장님 지시: C++·Java를 .NET 구조로, #2758 위반은 모두 고친다) — kairos 제안 15:02 UTC, zlink 동의.

**kairos (성능 경로)**: C++ V01·V05·V18, Java F3·F5·F7, .NET 구조로 맞추는 request 완료·수신 pump
**zlink**: C++ V02~V04·V06~V17·V19~V21, Java F4·F6·F8, .NET 01~08 전수 감사

zlink 묶음과 대상 파일(같은 파일을 두 사람이 동시에 고치지 않기 위해):
- 진행 중 C1 — V06·V15·V16: `streams/session_serial_executor.hpp`, `streams/stream_runtime.cpp`(control 부분), `actors/actor_gateway_runtime.{hpp,cpp}`, `fanout/fanout_location_runtime.{hpp,cpp}` (branch framework-cpp/2758-state-ownership)
- 다음 C2 — V17·V19·V20: `connector/core/src/runtime/calls/zlink_stream_calls.cpp`, `connector/core/src/runtime/connector_runtime.cpp`, `connector/engines/unreal/...ZLinkStreamConnector.cpp`
- #2616 머지 뒤: A — V02~V04(route_packet_dispatcher·route_handler_invoker·route_handler_registry·handlers/handler_registry), B — V07~V12(수동 ClientServer: channels·client_server), V13·V21(`route_channel_runtime.cpp`), V14(`spots/spot_runtime.cpp`)
- Java F4·F6·F8: `runtime/actors/ZLinkActorClientRuntime.java`(F4), Spring handler factory·`ZLinkHandlerActivator.java`(F6), `runtime/binding/ZLinkJavaRawMeshNode.java:6967-6980`(F8) — kairos의 F3·F5와 `ZLinkActorClientRuntime.java`·`ZLinkJavaRawServicePort.java`가 겹치므로 F4는 kairos F3 뒤에 한다
- 진행 중: .NET 감사(`D:/t/jobs/fx/cx/adotnet/`), #2749 osx 할당 시험(`D:/t/jobs/fx/cx/f2749/`)

🤖 Generated with [Claude Code](https://claude.com/claude-code)

=== zlink-systems 2026-10-26T15:27:53Z
Java F6·F8은 sass에게 넘겼습니다(zlink 몫, kairos F3·F5와 파일 겹침 없음). F4는 kairos F3 뒤에 zlink가 합니다.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
=== zlink-systems 2026-10-26T15:31:53Z
sass가 Java F6(handler·filter 생성 절차 재선택)·F8(authority 조회 실패를 SOURCE_OWNER_UNAVAILABLE로 바꿈)을 맡았습니다(zlink 지정, 10-27). 브랜치 framework-java/2758-construction-plan-ingest-error. 진단(결정 소유 표·.NET 대조·분류) 진행 중이며, 승인 뒤 수정 전 실패 시험부터 구현합니다. F8은 스펙 PR #2774(04 확인 실패 절)를 기준으로 합니다.
=== zlink-systems 2026-10-26T15:33:49Z
## .NET 실행 스펙 감사 (sol, origin/main 08c4e1dae5, 정적 대조)

독립 위반 후보 13건(N01~N13), 스펙 gap·충돌 4건(G01~G04)입니다. 규칙별 대조 316행 중 판정 보류가 151행이라 **전수 완료가 아닙니다**. .NET을 기준 구조로 쓰는 다른 언어 수정에서도 아래 위치는 그대로 따라가지 않습니다.

감독자 직접 확인(스펙 조항·코드를 열어 봄):
- **N01 확정**: `ZLinkWorkerCall.cs:35`·`ZLinkIoWorkerCall.cs:31`의 `Submit()`이 결과를 기다리지 않고 관찰만 건다. 스펙 `01-submit-and-completion.ko.md:50`은 .NET `Submit()`을 동기 blocking 종결자(결과까지 호출 thread를 막고, runtime 문맥에서는 `InvalidOperation`)로 정한다.
- **N02 확정**: `ZLinkIoWorkerCall.cs:56`이 pre-cancelled token을 보기 전에 `work(...)`를 호출한다. 스펙 `03-cancellation-and-shutdown.ko.md:42-44`은 pre-cancelled token이면 admission을 시작하지 않는다.
- N03~N13: 후보. 수정 job의 1단계 진단에서 확인한다.

| 번호 | 요지 | 위치 |
|---|---|---|
| N01 | worker `Submit()`이 blocking 종결자가 아님 | ZLinkWorkerCall.cs:35, ZLinkIoWorkerCall.cs:31 |
| N02 | I/O worker가 pre-cancel 확인 전 work 실행 | ZLinkIoWorkerCall.cs:48-56 |
| N03 | 업무 `TimeoutException`을 DeadlineExceeded로 분류 | ZLinkIoWorkerCall.cs:56-61 |
| N04 | runtime 종료를 ShuttingDown이 아닌 취소로 전달 | ZLinkWorkerCall.cs:122·207, ZLinkIoWorkerCall.cs:75 |
| N05 | `ZLinkStateLane.RunAsync(Func<ValueTask>)`만 재진입 검사 없음 | ZLinkStateLane.cs:158-165 |
| N06 | MeshNode Node-direct 수신이 serial queue/gate 없이 handler를 시작 | ZLinkMeshNodeRouteDispatcher.cs:231-269 |
| N07 | RouteMesh Channel record마다 TCS·WhenAll 완료 wrapper | ZLinkMeshNodeRouteDispatcher.cs:258-329, ZLinkMeshDispatchPump.cs:327 |
| N08 | Channel application queue drain에 시간 예산 없음(64건만) | ZLinkChannelApplicationDispatchQueue.cs:113-139 |
| N09 | completion table C2 상태를 lock으로 소유 | ZLinkMeshCompletionTable.cs:16-230 |
| N10 | ClientServer request reply가 중간 Task 연쇄를 거치고 늦은 reply는 typed codec을 지나지 않음 | ZLinkRequestSubmissionOutcome.cs, ZLinkRequestCompletion.cs:74 |
| N11 | BACKPRESSURED request마다 CTS·Task.Delay timer | ZLinkRequestSubmissionOutcome.cs:47-131 |
| N12 | completion dispatcher 문맥에서 blocking Submit을 거부하지 않음 | ZLinkApplicationExecutionContext.cs:34, ZLinkCompletionDispatcher.cs:25 |
| N13 | Spot request reply encode 전에 Defer Join 활성화 | ZLinkSpotHandlerInvoker.cs:60·80, ZLinkSpotRouteDispatcher.cs:160 |

스펙 gap: G01 Session one-way reply 표의 `.Async(ct)` 인자(S01 §15:561 ↔ §7·§8), G04 언어별 문서가 #2752 고정 worker pool을 반영하지 않음. G02·G03은 문서에 이미 미확정으로 표시된 08 §7 수치.

보고서 원문: 감독자 로컬 `D:/t/jobs/fx/cx/adotnet/report.md`, `rules.md`.

=== zlink-systems 2026-10-26T15:35:07Z
정정(zlink 10-27 15:34 결정): sass는 F6만 맡습니다. F8은 kairos가 F3·F4·F5·F7과 함께 처리합니다. sass의 F8 읽기 전용 진단 결과는 참고용으로만 올립니다.
=== zlink-systems 2026-10-26T15:39:13Z
**N14 (감독자 판정, kairos 질문)**: .NET direct request(`ZLinkManagedMeshNode.cs:10759 RequestDirectWireAsync` → `ZLinkRequestSubmissionOutcome.cs:33` → binding `CompletionOwner.cs:937`)는 Framework 진행 중 호출 표 등록·원자적 꺼내기·completion dispatcher 게시 없이 binding entry로 완료한다. 스펙 `01-submit-and-completion.ko.md:401`·`:422`(보내는 runtime이 OperationId·ReplyRouteId·pending entry·dispatcher 자리를 등록한 뒤에만 submit)과 `08-messaging-hot-path.ko.md:98` E3(request는 항상 등록)을 어긴다. binding entry는 Spot 종료·generation·deadline·OperationId 중복 제거를 경쟁시킬 수 없으므로 표를 대신하지 못한다. 이 결정은 C++·Java를 .NET 구조로 맞추는 기준에서 제외한다(C++ `operation_registry` 유지). N10·N11과 같은 작업에서 고친다.
=== zlink-systems 2026-10-26T15:42:27Z
## F6 진단(sass) — 분류 B, 대안 1 채택

감독자 판정: 대안 1(등록 환경별 단일 생성 plan, metadata로 알 수 있는 생성 불가는 startup 실패, dispatch는 재선택·대체 constructor 없이 정해진 plan만 실행). 근거 조항 직접 확인: 08-messaging-hot-path.ko.md:241-247, 06-framework-api.ko.md §23(1016: 생성자 인자 공급 가능성을 등록·metadata로 startup에 검사, 생성 실행 금지). 대안 2(실패 delegate로 첫 dispatch에서 실패)는 §23 위반이라 기각. 이 판정으로 수정 전 실패 시험부터 구현을 시작합니다. 이견이 있으면 이 이슈에 적어 주세요.

## F6 — 생성 절차의 반복 선택

### 결정 소유 표

| 결정 | 스펙 소유 조항 | 현재 판정 위치: Java·.NET·Node·C++ | 모을 소유자 |
|---|---|---|---|
| 어떤 constructor를 사용할지 | 등록·구성 준비에서 확정하고 재사용한다. `E/08-messaging-hot-path.ko.md:241` | Java: Spring 준비가 사용 가능한 후보를 찾지만 저장하지 않는다(`S/ZLinkSpringHandlerFactory.java:88`). 생성 때 다시 순회한다(`:292`). MutableServices도 생성 때 순회한다(`J/runtime/internal/handlers/ZLinkHandlerActivator.java:132`). 기본 reflection은 ClassValue 계산에서 no-arg를 선택한다(`:289`). .NET: catalog별 `SelectConstructor`와 factory 캐시(`D/Runtime/Handlers/ZLinkScopedHandlerInstanceOwner.cs:118`, `:160`). Node: 준비된 constructor와 resolver 배열(`NS/providers.ts:562`). C++: 컴파일 시 constructor arity 선택과 등록 factory(`C/include/zlink/framework/contracts/configuration/services.hpp:260`, `:320`). | 각 등록 환경의 activator가 준비하는 생성 절차 하나. Java는 Spring용 준비 위치 하나와 core용 준비 위치 하나로 한정한다. |
| dependency 연결 방법을 고를지, 현재 scope에서 값을 조회할지 | 연결 방법은 준비 때 고정하고 현재 scope의 dependency 조회만 허용한다. scope provider·instance를 공유 plan에 보관하지 않는다. `E/08-messaging-hot-path.ko.md:241` | Java: Spring metadata 검사(`S/ZLinkSpringHandlerFactory.java:104`), 생성 때 resolver·scope·shortcut·descriptor 경로 선택(`:312`, `:334`, `:365`), MutableServices의 매번 parameter reflection·fallback 연결(`J/runtime/internal/handlers/ZLinkHandlerActivator.java:210`). .NET: parameter key·기본값·GetService 식을 compile(`D/Runtime/Handlers/ZLinkScopedHandlerInstanceOwner.cs:241`). Node: constructor/property resolver를 준비하고 현재 context에 적용(`NS/providers.ts:566`, `:590`, `:613`). C++: dependency type을 template로 확정하고 현재 provider에서 조회(`C/include/zlink/framework/contracts/configuration/services.hpp:272`, `:289`). | activator의 동일 생성 절차가 연결 방법을 소유한다. activation은 현재 scope의 값을 공급하고 소유·정리한다. |
| 생성 불가를 언제 실패시킬지 | metadata로 알 수 있는 필수 dependency 누락·선택 모호성은 startup 실패다. 검사를 위한 생성은 금지한다. `API:1006`, `API:1016`, `API:1034` | Java: Spring은 준비 시 missing/ambiguous dependency를 거부한다(`S/ZLinkSpringHandlerFactory.java:94`, `:147`). 그러나 activation은 dependency뿐 아니라 constructor 실행 실패까지 다음 후보로 전환한다(`:298`). core prepare는 metadata만 읽고 no-arg 부재는 create에서 실패한다(`J/runtime/internal/handlers/ZLinkHandlerActivator.java:30`, `:302`, `:310`). .NET: compile 오류를 실패 delegate로 캐시하고 Resolve에서 던진다(`D/Runtime/Handlers/ZLinkScopedHandlerInstanceOwner.cs:137`). Node Nest: 준비 중 필수 token introspection 실패를 던진다(`NS/providers.ts:620`). C++: 주입 가능한 constructor 부재는 static_assert(`C/include/zlink/framework/contracts/configuration/services.hpp:266`), 실제 dependency 조회는 `:284`. | host 등록은 startup validator와 동일 plan 준비 결과로 실패한다. Spot.Configure에서 생긴 등록은 Ready 전 activation 검사에 속한다(`API:1036`). 실제 provider·constructor 실행 실패는 해당 activation/dispatch 실패로 처리하고 constructor를 재선택하지 않는다. |

### 원인과 교차언어 대조

Spring의 `HandlerPlan`은 선택된 constructor 하나가 아니라 정렬한 후보 전체를 보관한다. `prepare`가 성공한 후보와 `create`가 실제 선택하는 후보가 별개다. 또한 `BeansException`과 `ReflectiveOperationException`을 잡아 다음 후보를 시도하므로, 생성자 업무 코드의 예외도 다른 constructor 실행으로 이어질 수 있다. `S/ZLinkSpringHandlerFactory.java:83`, `:285`, `:298`, `:504`.

Channel invoker는 이 선택의 소유자가 아니다. dispatch마다 owner를 만들고 `handlers.instance`를 호출하는 수명 처리는 계약에 맞는다. owner 안의 instance 캐시는 dispatch 사이의 생성 절차 재사용을 대신하지 못한다. `J/runtime/channels/ZLinkChannelHandlerInvoker.java:495`, `:503`; `J/runtime/internal/handlers/ZLinkHandlerInstanceOwner.java:38`; `API:485`; `E/08-messaging-hot-path.ko.md:246`.

MutableServices의 직접 `create`와 channel의 activation은 구분해야 한다. 직접 `create`는 constructor를 순회하지만, `openActivation().create`는 등록 instance가 없으면 `fallbackActivation.create(..., resolver)`로 위임한다. 따라서 MutableServices의 직접 순회가 모든 channel dispatch에서 실행된다는 감사 표현은 이 HEAD에서 성립하지 않는다. 직접 생성의 실제 호출자는 session handler map 구성 등이다. `J/runtime/internal/handlers/ZLinkHandlerActivator.java:126`, `:168`, `:185`; `J/runtime/streams/ZLinkSessionPacketDispatcherRuntime.java:98`.

.NET의 참고점은 provider의 service catalog별 factory 캐시와 현재 scope provider를 인자로 받는 delegate다. 같은 handler type도 다른 등록 환경이면 다른 선택이 가능하다. 다만 실패 delegate를 그대로 Java의 startup 정책으로 복제하면 안 된다. 이 클래스의 지연 실패 구현과 별도로 API §23의 사전 검사를 만족해야 하며, .NET 전체 startup validator의 충족 여부는 이번 조사에서 확인하지 않았다. `D/Runtime/Handlers/ZLinkScopedHandlerInstanceOwner.cs:124`, `:137`, `:229`; `API:1016`.

Node 기본 생성은 `() => new type()`를 준비하고, Nest는 constructor·property resolver를 미리 만들어 등록 map에 저장한다. dispatch는 준비된 factory와 현재 context의 dependency map을 사용한다. 임의의 외부 provider resolver 내부까지 같은 비용 조건을 지키는지는 확인하지 못했다. `N/runtime/handlers/handler-instance-scope.ts:45`; `N/runtime/channels/channel-dispatch-services.ts:115`; `NS/providers.ts:412`, `:421`, `:536`, `:562`.

C++는 등록한 호출 lambda와 컴파일된 주입 표현을 사용한다. constructor arity를 record마다 reflection으로 다시 선택하지 않는다. 필수 service 누락을 startup에서 모두 검사하는지는 이 주입 코드만으로 확인하지 못했다. `C/include/zlink/framework/contracts/handlers/handler_registry.hpp:97`, `:342`; `C/include/zlink/framework/contracts/configuration/services.hpp:260`, `:284`, `:320`.

**분류: B 기존 결함.** 생성 절차 선택 시점과 dependency 수명은 이미 공통 스펙이 정했다. 언어별 constructor 문법 차이는 허용되지만 dispatch별 재선택은 허용되지 않는다. 새로운 public API나 스펙의 언어별 예외가 필요하지 않다. `E/08-messaging-hot-path.ko.md:241`, `:269`; `API:506`.

### 대안과 판정 위치 수

아래 수는 Java에서 constructor를 선택하는 **코드 위치 수**다. Spring prepare, Spring activation, MutableServices.create, core no-arg 선택의 네 곳을 센다. Handler 수·scope 수·캐시 entry 수는 세지 않는다. `S/ZLinkSpringHandlerFactory.java:88`, `:292`; `J/runtime/internal/handlers/ZLinkHandlerActivator.java:132`, `:289`.

| 대안 | 수정 전 → 후 | 추가·삭제 상태와 분기 | 판정 |
|---|---|---|---|
| 1. 등록 환경별 단일 plan을 준비하고 사전 불가를 즉시 거부 | constructor 선택 **4→2**: Spring plan compiler와 core plan compiler. dependency 연결 방법 선택 **3→2**: Spring 검증/activation의 중복과 MutableServices 연결을 각 compiler로 모음 | 후보 전체를 activation에서 보는 구조, `lastFailure`, constructor별 catch/fallback 순회를 삭제한다. 기존 plan 저장소를 선택된 constructor·인자 resolver로 교체한다. scope instance 저장소는 유지한다. runtime 선택용 flag·lock·queue는 추가하지 않는다. | **추천**. 준비·조회·수명 경계를 함께 만족한다. `E/08-messaging-hot-path.ko.md:241`; `API:1016`. |
| 2. 동일 plan을 준비하되 해석 불가를 실패 delegate로 저장하여 첫 dispatch에서 실패 | constructor 선택 **4→2**, dependency 연결 방법 선택 **3→2** | 후보 순회를 삭제하고 실패 원인을 보유한 delegate를 추가한다. dispatch에 실패 delegate 실행 경로가 남는다. | metadata로 확인한 누락을 지연하면 §23 위반이다. 명시적 application-owned factory 제외를 일반 handler에 확대하면 안 된다. `.NET` 참고 구조의 이 부분은 채택하지 않는다. `D/Runtime/Handlers/ZLinkScopedHandlerInstanceOwner.cs:146`; `API:1016`, `API:1034`. |

추천안에서 Spring plan은 선택된 constructor 또는 invoker, 각 인자의 확정된 service key/qualifier·optional·collection 연결 방법, runtime context 인자 위치, 기존 Spring 초기화·정리 metadata를 가진다. Spring이 소유하는 후보 선택·주입·초기화 정책을 별도의 매핑표로 복제하지 않고 기존 Spring metadata를 사용해야 한다. 현재 constructor 후보 배열과 점진적으로 변경되는 shortcut 저장 방식을 그대로 두어서는 안 된다. `S/ZLinkSpringHandlerFactory.java:104`, `:295`, `:334`, `:456`, `:539`.

Core plan은 기본 reflection과 MutableServices의 동일 생성 결정을 한 compiler가 소유하게 한다. 선택된 constructor, 미리 얻은 parameter type/key, 등록 service·runtime 제공 인자·fallback 공급 경로만 담는다. 특정 scope provider와 instance는 담지 않는다. 임의 callback의 내부를 추측해 검사하지 않으며, §23이 제외한 명시적 factory 등록은 그 callback 자체를 생성 절차로 재사용한다. `J/runtime/internal/handlers/ZLinkHandlerActivator.java:25`, `:109`, `:210`, `:284`; `API:1016`; `E/08-messaging-hot-path.ko.md:245`.

준비는 Framework service 구성이 끝난 등록 환경에 대해 해야 한다. 현재 `handlerFactory.prepare`는 runtime용 MutableServices 생성·추가보다 먼저 실행된다. 이를 무시하고 ClassValue에 최종 DI 선택을 저장하면 host별 service 차이를 잃는다. type별 reflection metadata와 등록 환경별 생성 plan은 구분하되 constructor 선택의 소유자는 하나로 둔다. `J/runtime/host/ZLinkFrameworkRuntime.java:213`, `:218`, `:220`; `J/runtime/internal/handlers/ZLinkHandlerActivator.java:109`; `D/Runtime/Handlers/ZLinkScopedHandlerInstanceOwner.cs:127`.

Dispatch는 새 scope 생성, 고정 key로 현재 dependency 조회, 정해진 invoker 한 번 실행, Spring의 기존 초기화, terminal에서 정리만 한다. 생성 실패 시 다른 constructor를 시도하지 않는다. 생성 불가를 metadata로 판정할 수 있으면 startup에서 확정하고, 실제 provider·constructor 실행 중 실패는 해당 dispatch에서 전달한다. Spot·Actor instance 생성은 activation scope에서 수행한다. `API:485`, `API:510`, `API:520`, `API:1016`; `E/08-messaging-hot-path.ko.md:248`, `:265`.

### 수정 전 실패 시험 설계와 확인 결과

1. **공개 channel request를 통한 Spring 재선택 시험.** `ST/ZLinkSpringHandlerFactoryTest.java`의 counting bean factory를 이용하는 새 통합 시험을 설계한다. 실제 호출은 public handler 등록과 channel request를 사용한다. Handler와 filter 각각에 우선순위가 높은 해석 불가 constructor와 해석 가능한 constructor를 둔다. 준비 후 missing dependency type의 조회 횟수를 초기화하고 순차 request N개를 보낸다. 기대값은 missing type 조회 **0**, 선택한 constructor 호출 **N**, reply **N개**다. 현재는 missing 후보 조회가 N에 비례한다. 전체 provider 조회 0을 요구하면 정상 scoped 조회까지 금지하므로, 탈락 후보의 조회만 센다. `S/ZLinkSpringHandlerFactory.java:88`, `:292`, `:318`, `:365`; `ST/ZLinkSpringHandlerFactoryTest.java:117`, `:276`.
2. **실패를 다른 constructor로 숨기는지 확인.** 등록 metadata상 해석 가능한 우선 constructor의 본문이 예외를 던지게 하고, 다른 constructor는 성공하게 한다. 기대값은 request 실패·대체 constructor 실행 0이다. 현재는 InvocationTargetException을 잡고 다음 후보로 넘어간다. 정상 constructor만 둔 대조군은 reply를 받아야 한다. `S/ZLinkSpringHandlerFactory.java:295`, `:298`.
3. **startup와 수명 확인.** 필수 dependency를 공급할 수 있는 constructor가 하나도 없으면 첫 request 전에 host 시작이 실패해야 한다. 검사 중 handler·dependency 생성 횟수는 0이어야 한다. 정상군은 handler와 filter가 같은 dispatch dependency를 사용하고 다음 dispatch에서는 다른 instance를 사용해야 한다. Spot.Configure 등록은 Ready 전 실패를 검사한다. `API:485`, `API:1016`, `API:1036`.
4. **core 직접 생성의 보조 재현을 실행했다.** 원본 activator와 오류 class 네 파일만 `/tmp/d2758/probe/src`에 복사하여 javac로 컴파일했다. Known만 등록하고 Missing을 해석하지 못하는 constructor를 가진 handler를 `prepare`한 뒤 직접 `create`를 3회 호출했다. 탈락 dependency 재조회 0을 요구한 시험은 **실제 3으로 실패(exit 1)**했다. 모든 dependency를 등록한 대조군은 fallback 조회 **0으로 통과(exit 0)**했다. 이는 MutableServices 직접 생성의 재탐색 증거이며 공개 channel 재현을 대신하지 않는다. `/tmp/d2758/probe/Probe.java:14`, `:19`, `:20`; 원인 `J/runtime/internal/handlers/ZLinkHandlerActivator.java:132`, `:210`.

공개 channel·Spring 통합 시험은 설계만 했으며 실행하지 않았다. 현재 `JT/runtime/channels/ZLinkHandlerPreparationTest.java:16`은 handler 업무 메서드 선택을 검사하고, `ST/ZLinkSpringHandlerFactoryTest.java:117`은 단일 성공 constructor의 준비를 검사한다. 이 시험들만으로 탈락 constructor의 반복 조회를 검증했다고 볼 수 없다.

### 성능 영향

- Spring은 후보마다 인자 배열을 만들고 필요하면 HashSet과 DependencyDescriptor를 만든다. 실패한 후보는 Spring 예외와 Framework의 IllegalStateException wrapper도 만들 수 있다. 선택 plan을 고정하면 탈락 후보의 조회·배열·예외 비용이 사라진다. 선택한 constructor의 인자 배열과 신규 scope·dependency·handler 할당은 별개다. `S/ZLinkSpringHandlerFactory.java:294`, `:298`, `:315`, `:334`.
- MutableServices 직접 생성은 후보마다 `getParameterTypes()`와 Object[] 생성, 등록 service 존재 검사, 실제 조회를 반복한다. core reflection의 constructor 목록 자체는 이미 ClassValue에 캐시되어 있으므로 매번 `getConstructors()`를 호출한다고 판단하면 안 된다. `J/runtime/internal/handlers/ZLinkHandlerActivator.java:210`, `:214`, `:220`, `:286`.
- 준비된 Constructor.newInstance 자체는 스펙이 금지한 constructor 재선택과 다르다. 이를 없애기 위해 MethodHandle 등으로 바꿔야 한다는 근거는 없다. 절감 byte 수·시간·처리량은 측정하지 않았다. `E/08-messaging-hot-path.ko.md:243`; `S/ZLinkSpringHandlerFactory.java:295`.


=== zlink-systems 2026-10-26T15:42:29Z
## F8 진단(sass, 읽기 전용) — kairos 참고용

F8은 zlink 결정(10-27 15:34)으로 kairos가 맡습니다. sass는 코드를 바꾸지 않았고, 아래는 같은 진단 job의 F8 결과입니다.

## F8 — authority 확인 실패의 분류 소실

### 결정 소유 표

| 결정 | 스펙 소유 조항 | 현재 판정 위치: Java·.NET·Node·C++ | 모을 소유자 |
|---|---|---|---|
| 확인 실패를 어떤 결과로 전달할지 | 예상 밖 오류·취소를 owner 없음으로 바꾸지 않는다. `P04:222`; request 분류 `ERR:101`, `ERR:142` | Java: Actor에서 failure와 empty를 합쳐 Unavailable로 보냄(`J/runtime/binding/ZLinkJavaRawMeshNode.java:6989`). Spot도 동일(`:5950`). 기존 공통 변환기는 `:2602`와 `J/runtime/internal/backend/ZLinkRequestFailureMapping.java:19`. .NET: resolver는 특정 Unavailable만 null로 변환(`D/Runtime/Locations/ZLinkRequestSourceFenceResolver.cs:61`), 예상 밖 오류는 pump의 batch 바깥 catch로 전달(`D/Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:517`, `:562`). Node: typed 오류는 reply하고 예상 밖 오류는 rethrow(`N/runtime/foundation/service-stateful-runtime.ts:4195`). C++: resolver는 비-Unavailable을 rethrow하지만 ingest가 catch-all로 conflict로 변경(`C/src/runtime/stateful/raw_stateful_dispatch.cpp:1086`, `:707`, `:774`). | 오류 분류는 공통 오류 모델과 기존 failure mapper. ingress는 원래 failure와 owner 없음의 차이를 보존하고 메시지별 terminal을 소유한다. |
| 실패한 message를 owner queue에 넣을지 | 실패한 message는 넣지 않고 sequence도 바꾸지 않는다. `P04:218`, `P04:222` | Java: 확인 실패 return이 enqueueRemoteActor보다 앞선다(`J/runtime/binding/ZLinkJavaRawMeshNode.java:6989`, `:7000`). Spot도 앞선다(`:5950`, `:5983`). .NET: 확인 뒤 DispatchRecord→EnqueueActor(`D/Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:517`, `:524`, `:621`). Node: raw mailbox enqueue 후 sourcePreparationOwner가 dispatch 준비에서 확인(`N/runtime/foundation/service-stateful-runtime.ts:4213`, `:4247`, `:4263`). C++: canonical mailbox slot에서 확인하고 실패 시 original을 take하여 terminal, 성공 시 후속 수락(`C/src/runtime/stateful/raw_stateful_dispatch.cpp:626`, `:674`, `:742`). | owner ingress 수락 경계 하나. raw transport mailbox와 최종 application owner queue는 구분한다. 실패 시 handler 호출·수락 sequence 증가가 없어야 한다. |
| 같은 claim의 다음 message를 계속할지 | message별 계속, host·unit admission 닫힘만 공통 종료. `P04:225` | Java: 비동기 실패는 callback의 return으로 끝나고 receive loop는 다음 record로 진행(`J/runtime/binding/ZLinkJavaRawMeshNode.java:6997`, `:7092`, `:4906`, `:4971`). 동기 resolver 예외는 `:7086`에서 rethrow. .NET: record loop 밖 catch여서 다음 record 진행을 보장하지 않음(`D/Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:513`, `:560`). Node: 실패 record 뒤 state.dispatchRecords 재개(`N/runtime/backend/mesh-dispatch-pump.ts:575`). C++: ingest 결과 뒤 batch index 증가·계속(`C/src/runtime/stateful/public_host_runtime.cpp:7978`, `:7986`, `:7997`, `:8231`). | claim/receive 순회를 소유하는 ingress. message 실패 terminal 이후 다음 record를 진행하고 admission 종료만 별도로 적용한다. |

### 원인과 교차언어 대조

Java의 실제 오류 합류점은 감사가 제시한 줄 번호가 아니라 **6989행**이다. `resolveAcceptedAuthorities`는 source와 local target 조회를 thenCombine으로 합치고, 둘 중 하나의 exceptional completion도 전달한다. 이를 받은 dispatchActor가 실패의 종류를 보지 않고 Unavailable로 바꾼다. `J/runtime/binding/ZLinkJavaRawMeshNode.java:6984`, `:6989`, `:7107`, `:7116`.

.NET은 resolver 경계에서 예상 밖 오류를 owner 없음으로 바꾸지는 않는다. 그러나 pump의 await는 record별 catch 안에 없어서 오류가 claim 나머지 처리를 중단시킬 수 있다. 따라서 .NET의 resolver 구분은 참고할 수 있지만 claim 처리 전체를 정상 기준으로 삼을 수 없다. `D/Runtime/Locations/ZLinkRequestSourceFenceResolver.cs:61`; `D/Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:513`, `:562`.

Node는 typed 오류를 failure mapper로 보내고, 임의 오류는 pump에서 record 실패로 기록한 뒤 다음 record로 진행한다. 이 경로에서는 예상 밖 오류에 대한 request error reply를 확인하지 못했다. 또한 authority 확인은 raw mailbox에 들어간 뒤 준비 단계에서 실행된다. 이 raw mailbox가 P04가 말하는 application owner queue와 같은 수락 경계인지는 추가 확인이 필요하다. `N/runtime/foundation/service-stateful-runtime.ts:4195`, `:4247`; `N/runtime/backend/mesh-dispatch-pump.ts:244`, `:575`.

C++는 resolver에서 일반 오류를 다시 던지지만 ingest가 conflict로 바꾸고 requestRejected로 응답한다. Java의 Unavailable과도 다르며, 원래 오류 분류를 유지하지 않는다. 해당 오류는 stateful 결과로 반환되므로 다음 batch index는 진행한다. `C/src/runtime/stateful/raw_stateful_dispatch.cpp:1089`, `:707`, `:774`, `:783`; `C/src/runtime/stateful/public_host_runtime.cpp:7978`, `:7987`.

**분류: B 기존 결함.** 예상 밖 Framework 실행 실패는 InternalFailure라는 기존 분류가 있으며 owner 부재와 다르다. P04는 message별 실패·계속 처리를 명시한다. 언어별 현재 동작 차이는 이 규칙의 재량이 아니다. 이번 Java 범위에서 확인한 Spot의 같은 오류 변환도 동일 원인으로 함께 처리해야 한다. `.NET`·Node·C++의 확인된 차이도 같은 #2758 작업 범위의 대조 결과로 남긴다. `ERR:106`, `ERR:111`; `P04:222`; `J/runtime/binding/ZLinkJavaRawMeshNode.java:5950`.

**취소 전달의 미확정 경계:** 현재 relayedFailurePair는 CancellationException도 일반 fallback인 InternalFailure로 바꾼다. wire terminal에는 cancelled 값이 없고 수신 result mapper에도 취소 분기가 없다. 따라서 단순히 이 mapper로 교체하는 것만으로 “authority 조회 취소 → 원격 caller cancelled awaitable”까지 만족한다고 판단할 수 없다. 이 하위 결정은 **D 스펙 gap 후보**이며, caller 자체 취소와 receiver 내부 조회 취소의 관계·전달 방식을 확인하기 전에는 완료 판정을 할 수 없다. 새 Java 전용 wire code를 추가하는 방안은 제외한다. `J/runtime/binding/ZLinkJavaRawMeshNode.java:2602`, `:2627`; `J/runtime/internal/backend/ZLinkBackendRequestResult.java:37`; `W:2753`; `ERR:142`.

### 대안과 판정 위치 수

여기서 세는 것은 Java Actor·Spot의 두 고정 Unavailable 판정과 기존 공통 오류 변환기다. 정상 owner 없음 판정과 메시지별 reply 전달은 없애지 않는다. `J/runtime/binding/ZLinkJavaRawMeshNode.java:5950`, `:6989`, `:2602`.

| 대안 | 수정 전 → 후 | 추가·삭제 상태와 분기 | 판정 |
|---|---|---|---|
| 1. 원래 failure를 공통 변환·완료 경계로 전달하고 owner 없음만 별도로 처리 | 오류 분류 판정 **3→1**. owner 부재 판정 **2→2**. receive 순회 소유자 **1→1** | 두 failure-or-empty 합류 판정을 제거한다. 실패와 정상 empty의 분기는 구분한다. 기존 exceptional completion에 오류를 유지하며 별도 오류 DTO·flag·queue를 추가하지 않는다. | **추천**. 예외 경로를 먼저 수정하고, 취소 전달은 위 미확정 경계를 해소한 뒤 같은 owner에 포함한다. |
| 2. resolver가 실패를 담은 새 outcome을 정상 완료로 반환하고 Actor·Spot에서 해석 | 공통 변환기를 쓰면 오류 분류 **3→1**, owner 부재 **2→2** | 기존 CompletionStage의 실패 상태 외에 성공·empty·error·cancel outcome과 각 소비자의 해석 분기를 추가한다. | 같은 실패 사실을 두 표현으로 유지한다. 규칙 수를 줄이지 못하므로 비추천. 현재 Stage가 이미 오류를 전달한다. `J/runtime/binding/ZLinkJavaRawMeshNode.java:7116`. |

추천 경로는 `resolveAcceptedAuthorities(:7107)`의 원래 오류 → 기존 분류 경계 `relayedFailurePair(:2602)`/`ZLinkRequestFailureMapping` → `replyActorFailure(:7269)` → wire reply → `ZLinkBackendRequestResult.toFrameworkErrorKind(:59)`다. typed kind·cause code는 기존 표를 사용하고, 일반 예상 밖 오류는 InternalFailure로 처리한다. 이미 있는 relay 실패 경로도 이 mapper를 사용한다. `J/runtime/binding/ZLinkJavaRawMeshNode.java:7066`; `J/runtime/internal/backend/ZLinkRequestFailureMapping.java:53`.

단, 동기 throw와 비동기 completion 실패를 같은 메시지 terminal 경계가 받아야 한다. 현재 `catch (:7086)`는 permit만 정리한 뒤 rethrow하므로, 비동기 failure 분기만 고치면 같은 batch 계속 조건을 충족하지 못한다. 원래 오류를 메시지별로 완료하고 payload·inbound·permit을 정리한 뒤 receive loop의 다음 iteration으로 복귀해야 한다. Actor와 Spot에 오류 분류표를 각각 추가하지 않는다. `J/runtime/binding/ZLinkJavaRawMeshNode.java:7086`, `:7079`, `:4966`, `:4971`.

실패한 message는 `enqueueRemoteActor`에 진입하지 않고, 정상 메시지에서만 현재 수락 경로를 사용한다. one-way는 기존 `replyActorFailure`가 기록 경로로 분리하지만 원래 Throwable 진단 정보도 기존 flow/error 기록에 보존해야 한다. 결과 pair만 바꾸면 원인 예외가 충분히 기록되는지는 확인하지 못했다. `J/runtime/binding/ZLinkJavaRawMeshNode.java:6990`, `:7000`, `:7274`; `ERR:95`; `P04:224`.

### 수정 전 실패 시험 설계

시험 위치는 새 `JT/runtime/binding/ZLinkJavaRawMeshNodeAuthorityFailureContractTest.java`를 제안한다. 기존 source-owner 시험은 resolver를 직접 호출하므로 public request 결과 검증을 대체하지 못한다. `JT/runtime/locations/ZLinkSourceRequestOwnerContractTest.java:87`, `:111`.

- **공개 API와 입력:** public Location Store SPI를 정상 provider에 위임하는 wrapper로 등록하고, 정상 startup·peer admission 뒤 receiver authority 조회에 해당하는 scan/read만 제어한다. 두 host와 실제 public Actor request를 사용한다. 내부 resolver setter·raw frame 생성·reflection 호출을 재현 API로 사용하지 않는다. SPI 위치는 `framework/languages/java/zlink-framework-provider-abstractions/src/main/java/systems/zlink/framework/locationprovider/ZLinkLocationStore.java:5`, 등록 API는 `J/configuration/ZLinkFrameworkOptions.java:34`다.
- **예상 밖 실패:** 해당 descriptor scan에 요청 pageSize보다 많은 row를 반환한다. Framework decode 뒤 IllegalStateException이 나므로 기대 request kind는 InternalFailure, handler 호출은 0이다. 별도 typed InternalFailure·ProtocolError 실패 stage도 대조한다. 현재는 Unavailable로 바뀐다. 일반 provider IllegalStateException을 그대로 던지는 시험은 provider boundary가 Store 실패로 분류할 수 있으므로 이 재현과 구분한다. `J/runtime/internal/locations/ZLinkProviderDescriptorRepository.java:500`, `:508`; `J/runtime/internal/locations/ZLinkProviderLocationRepository.java:435`; `J/runtime/binding/ZLinkJavaRawMeshNode.java:6989`.
- **같은 claim의 다음 message:** 첫 조회 stage를 제어하여 실패시키고 뒤의 정상 request를 같은 receive rotation/claim에 준비한다. 첫 request만 위 kind로 실패하고 다음 request는 정상 reply·handler 1회를 관찰해야 한다. 동기 throw·비동기 failure 두 경우를 검사하고, 성공 대조군과 정상 owner 부재→Unavailable 대조군을 둔다. 동일 claim에 들어갔다는 사실은 기존 trace 또는 읽기 전용 관측으로 확인해야 하며, 단순 동시 송신만으로 같은 claim이라고 판단하지 않는다. Java는 port.receiveNow로 record를 하나씩 claim하므로 네 언어의 claim 단위를 동일한 객체로 가정하지 않는다. `J/runtime/binding/ZLinkJavaRawMeshNode.java:4894`, `:4937`, `:5094`, `:7086`; `P04:225`.
- **취소:** receiver의 확인 stage 자체가 취소된 경우 기대값은 P04/오류 모델이 정한 취소 전달이며 Unavailable로 바뀌면 실패다. caller 자체 취소는 public request future 취소로 별도 대조한다. 그러나 현재 공개 SPI의 descriptor scan은 항상 false인 cancellation token을 받고, ProviderBoundary는 이 신호가 없는 spontaneous CancellationException을 Store 실패로 감싼다. 따라서 공개 API만으로 receiver authority 취소를 원형대로 주입하고 caller cancellation까지 관찰하는 구체 경로는 **확인 못 함**이다. 내부 resolver 직접 취소 시험으로 이를 대체했다고 보고해서는 안 된다. `J/runtime/internal/locations/ZLinkProviderDescriptorRepository.java:1105`; `J/runtime/internal/locations/ZLinkProviderLocationRepository.java:440`; `J/runtime/internal/calls/ZLinkRequestStages.java:43`; `ERR:142`.
- **one-way와 자원:** 같은 실패를 이미 admission 완료된 public send로 발생시켜 결과 재완료 없이 error 기록만 남는지 확인한다. 실패 뒤 payload 정리와 후속 request 진행을 확인한다. handler 미실행만으로 application owner queue에 전혀 들어가지 않았다고 단정하지 않고 sequence·기존 관측을 함께 확인한다. `ERR:95`; `P04:218`; `J/runtime/binding/ZLinkJavaRawMeshNode.java:6990`, `:7079`, `:7274`.

F8 공개 API 재현·교차언어 실행 시험은 수행하지 않았다. 위 실패 분류 소실과 언어별 제어 경로는 소스 대조 결과이며, 실행 통과로 판정한 결과가 아니다.

### 성능 영향

F8 자체의 authority 경로에서 reflection 호출은 확인되지 않았다. Actor는 확인 전에 application message를 decode하고, QueuedOwnership·completion callback과 whenCompleteAsync 실행을 준비한다. thenCombine도 기존 Stage 연결이다. 오류 분류 수정만으로 이 정상 경로의 할당·executor 전환이 사라지지는 않는다. `J/runtime/binding/ZLinkJavaRawMeshNode.java:6961`, `:6981`, `:6985`, `:7085`, `:7116`.

현재 owner 부재는 static int[]를 재사용하지만, 기존 일반 mapper는 오류 reply마다 새 int[2]를 만들 수 있다. 새 예외 wrapper를 추가할 필요는 없으며, 이미 발생한 Throwable을 분류에 사용한다. typed 오류와 일반 오류를 보존하는 비용은 실패 경로에 한정되고 성공 메시지당 추가 상태는 필요하지 않다. 실제 할당 byte·reflection의 간접 호출·처리량은 측정하지 않았다. `J/runtime/binding/ZLinkJavaRawMeshNode.java:146`, `:2608`, `:2621`, `:2627`.

## 검증 범위와 남은 항목

저장소 파일 변경은 없다. `/tmp/d2758`에서만 보조 probe를 컴파일·실행했고, 결함을 검출하는 실패와 정상 대조군 통과를 확인했다. 전체 빌드·시험은 실행하지 않았다. 남은 항목은 공개 API 통합 재현, F8 원격 취소 표현, 네 언어의 message별 실패·계속 계약 실행 검증이다. 진단 단계이므로 고친 항목은 0건이다. `/tmp/d2758/probe/Probe.java:19`, `:23`; `ERR:142`; `P04:222`.

점검 집계는 서로 겹치는 관점을 별도로 센다. 성능 1건은 F6 탈락 constructor 반복 조회, POSDDD 2건은 F6 생성 선택의 중복과 F8 오류 분류의 ingress 재판정, 불필요 코드 1건은 F6의 dispatch별 후보 fallback 절차다. `S/ZLinkSpringHandlerFactory.java:88`, `:292`, `:298`; `J/runtime/internal/handlers/ZLinkHandlerActivator.java:132`; `J/runtime/binding/ZLinkJavaRawMeshNode.java:6989`.

리팩토링 점검: 성능 1건·POSDDD 2건·불필요 코드 1건 발견
=== zlink-systems 2026-10-26T16:03:11Z
**C++ V06·V15·V16 수정 job 결과 리뷰 (감독자)** — branch `framework-cpp/2758-state-ownership`, 아직 commit 전.
- V06 승인: Session control을 lifecycle lane으로 게시(`session_serial_executor.hpp`), 시험 control 2건이 쌓인 application 작업보다 먼저 실행.
- V15 승인: `session_actor_binding_context_t::mutex` 제거, binding context를 gateway state lane 하나가 소유(판정 위치 18→9). native binder·registrar는 lane 밖.
- V16 반려·재작업 중: fanout의 실행 loop·pump·reconcile이 회전마다 `_lane.run().get()`으로 lane 완료를 막고 기다리고, 판정 위치가 4→5로 늘었다. loop가 기다리지 않는 구조로 다시 맡겼다(job f2758c1b). 끝나면 세 항목을 한 PR로 올린다.
=== zlink-systems 2026-10-26T16:07:39Z
N01·N02·N03·N04·N12 → PR #2785. Java 후보(N04와 같은 차이): `DefaultZLinkWorkerCall.java:87`·`DefaultZLinkIoWorkerCall.java:77`이 pool 종료를 ShuttingDown 대신 cancellation으로 전달한다(오류 모델 §2·§5). Java 묶음(kairos)에서 확인 바랍니다.
=== zlink-systems 2026-10-26T16:35:13Z
**감독자 판정(kairos 질문)**: target 쪽 authority 확인이 취소로 끝났을 때의 원격 분류는 오류 모델 §5가 원인으로 정한다 — host close·owner 정지로 인한 취소는 `ShuttingDown`(wire `terminated`, §2.1). §5 끝의 cancellation 문장은 호출자 자신의 취소에만 적용된다. C++(`request_failure_mapper.cpp:119-122`)·Java(F8)가 맞고 .NET(`ZLinkRequestFailureMapper.cs:229-236` InternalFailure)·Node(`service-stateful-runtime.ts:5923-5930` RequestFailed)를 고친다. 다른 취소 원천(예: relocation으로 owner 변경)이 있으면 그 경우는 `Unavailable`. kairos가 네 언어를 함께 맞춘다.
=== zlink-systems 2026-10-26T17:05:37Z
후속 감사 항목(C++, #2791 리뷰에서): `location_auto_connect_host_service.hpp:375,417-426` auto-connect host loop가 `republish_after_store_recovery`로 mesh·fanout·ClientServer의 descriptor Store 쓰기 완료를 동기로 기다린다. infrastructure loop가 Store 완료를 막고 기다리는 구조인지 08 §4.2·06 §5 기준으로 판정 필요.
=== zlink-systems 2026-10-26T17:25:57Z
**C++ connector V17·V19·V20 리뷰 (감독자)** — branch `framework-cpp/2758-connector`, commit 전. 시험은 connector 39/39, unit·contract 200/200까지 왔다(check 371·374는 스펙 §7에 맞춰 정렬 — 분류 A).
반려 3건, 재작업 job 진행 중:
1. Manual `dispatch_pending`(V20)이 packet마다 `make_shared` envelope과 handler마다 `std::function` 둘을 만든다 — 기본 mode의 메시지당 할당. connector state의 cursor(진행 중 packet + 다음 handler 위치)로 한 callback씩 실행하도록 다시.
2. 영어 주석을 한국어로 바꾸고 여전히 맞는 설명(§5.2 read pump 유일 reader 등)을 지웠다 — 되돌림.
3. 비동기 wait 결과가 모든 connector가 공유하는 I/O worker 4개 위에서 사용자 callback을 실행한다 — 스펙 §7·§10.1.1과 .NET 대조 후 판정.
=== zlink-systems 2026-10-26T22:58:45Z
V14 후속(명시적 close의 closing deadline): `languages/cpp/interfaces/04-spots.ko.md:427`은 closing context의 `deadline`을 "closing operation의 absolute UTC time"으로 정한다. 명시적 close의 closing operation은 Close 호출이므로 deadline은 그 operation의 deadline(호출자가 주지 않으면 기본 request timeout)이다. 언어별 현재 값: .NET `DateTimeOffset.UtcNow + DefaultRequestTimeout`(준수), C++ `system_clock::time_point::max()`(위반), Java `Instant.now()`(위반 — callback 시작 전에 이미 지난 deadline). 스펙 빈칸이 아니므로 C++·Java를 .NET과 같게 고친다. C++ host shutdown 경로는 `framework-cpp/2758-spot-closing`에서 고치는 중이다.
=== zlink-systems 2026-10-26T22:59:09Z
추가: Node 명시적 close는 `spot-closing.ts`의 별도 상수 `DEFAULT_CLEANUP_TIMEOUT_MS = 30_000`을 closing deadline으로 쓴다. 스펙상 deadline은 closing operation(Close 호출)의 deadline이므로, .NET처럼 Close operation의 request deadline(설정된 기본 request timeout)을 써야 한다. Node도 같은 묶음에서 맞춘다(C++·Java·Node, 기준 .NET).
=== zlink-systems 2026-10-27T00:17:58Z
V09 후속(같은 Channel 자기 요청 대기): `01-execution/02-handler-turn-and-execution-gate.ko.md` §4(표 `:297-298`)는 현재 turn을 유지한 채 같은 gate가 필요한 결과를 기다리는 public 호출을 제출 전에 `InvalidOperation`으로 거부한다고 정한다. Channel handler가 같은 Channel(같은 gate)로 request를 보내고 기다리면 이 규칙의 대상이다. C++(V09 직렬화 뒤)와 .NET은 지금 거부하지 않고 그 자리에서 멈춘다(kairos 대조). Java·Node를 대조한 뒤 네 언어가 제출 전 거부하도록 이 이슈에서 고친다. C++ V07~V12 PR은 시험을 다른 Channel로의 nested request로 바꾼다.
=== zlink-systems 2026-10-27T02:30:40Z
**F9(Java) one-way send timeout — 01 §7 위반, #2758에 넣는다**
- 스펙: `01-submit-and-completion.ko.md` §7 — one-way send에는 시간 상한과 caller cancellation이 없고, route resolve·ready 대기처럼 binding operation 전 단계도 같다. 끝나는 사건은 admission·route 소실(`Unavailable`)·close/shutdown(`ShuttingDown`)뿐이다.
- Java: `ZLinkChannelRuntime.java:1348` `sendToSpotViaRouterChannel`이 `sockets.submitToSpot(..., defaultRequestTimeout, ...)`과 `callRuntime.submit(timeout, ...)`로 send에 request timeout을 건다.
- 다른 언어(kairos 대조): .NET `ZLinkClientServerClientRuntime.cs:154 SendAsync`, Node `router-socket-spot-route-transport.ts:14`는 timeout 없이 기다린다(준수). C++는 #2857에서 같은 구조로 맞춘다.
- 수정은 같은 파일을 고치는 #2815(kairos)에 넣는다. 시험: send가 timeout 뒤에도 끝나지 않고 route 소실에 `Unavailable`, close에 `ShuttingDown`으로만 끝난다(수정 전 실패).
=== zlink-systems 2026-10-27T02:44:14Z
**T1(네 언어) message-flow telemetry provider를 worker가 동기로 기다린다 — `06-observability/03-message-flow-tracing.ko.md` §6 :291-294 위반**
- 스펙: "Worker는 느리거나 실패한 telemetry provider를 기다리지 않는다. 크기가 제한된 telemetry queue가 가득 차면 정상 trace를 버리고 `zlink.observability.events.overflow`를 증가시킬 수 있다."
- 현재: 네 언어 모두 provider를 호출 thread에서 동기로 부른다(Java `ZLinkMessageFlowTracer.traceAtMode`·`logDefault`, .NET `TraceMessageFlow`, Node span 생성, C++ message-flow tracer). Java에서는 느린 JUL provider가 독립 deadline scheduler를 멈추는 것을 통제 실험으로 재현했다(#2471 진단 `D:/t/jobs/fx/cx/d2471jv/report.md`).
- 수정: 네 언어에 process telemetry 전달 소유자 하나(제한된 queue, 포화 시 새 기록 폐기·overflow metric, provider failure는 fallback logger로 횟수 제한)를 같은 작업으로 둔다. 교차언어 contract test: provider를 막은 동안 message terminal과 deadline이 진행한다.
- #2471의 Java 원인 보존 수정과는 분리한다.
=== zlink-systems 2026-10-27T16:21:09Z
F6(Java handler·filter 생성 절차를 등록 환경별로 한 번 정해 재사용, ingest 오류)는 #2801로 main에 들어갔습니다(2026-10-27 15:51 UTC). worktree·브랜치·job 폴더를 정리했습니다.
=== zlink-systems 2026-10-28T13:38:46Z
**PM 상태 갱신 — 2026-10-28 13:38 UTC**

오늘 00:00 UTC 이후 실제 PR 머지 69건, 열린 PR 9건입니다. 중복으로 닫은 #2992·#3014는 머지 건수에 포함하지 않습니다. 전체 PR별 상태는 [프로젝트 개요](https://github.com/users/zlink-systems/projects/1)에 기록했습니다.

이 이슈는 `In Progress`를 유지합니다. 관련 변경 일부가 머지됐다는 이유로 감사 전체를 완료 처리하지 않습니다.

- 머지: #2815·#2791·#2885·#2900 및 Java queued permit 보완 #3132. #3132의 최종 머지는 12:51:06 UTC, main은 `6b286485bb9e7499d7220b8a1a04fc192bffcfde`입니다.
- 남은 C++: #2958은 공유 pool 동기 대기 수정의 로컬 근거를 확보했지만 최신 main 최종본 전체 리뷰·Linux gate·샘플·교차언어 검증이 미완료입니다. #2942는 deadline/registry/physical reply 정리의 최종 검토 중이며, #2857은 #2942의 기존 deadline helper 제거와 통합한 뒤 검증해야 합니다.
- 남은 Node: #2911은 logical entry·늦은 reply 정리·fallback·최초 deadline·codec 실행 전 terminal 확정 순서의 검토가 남았습니다. #2843은 선행 #2609 이후 통합합니다.
- #2609의 public typed remote 경로 비용 실측은 100요청당 Framework Promise 200개입니다. 원래 비용1 기준 미충족을 보존하며, binding API 재사용과 추가 owner의 필요성을 확인하기 전 기준2를 승인하지 않습니다. raw standalone 경로 측정과 같은 경계로 합치지 않습니다.

로컬 build/focused/private 접근/format 통과는 현재 최종본의 전체 gate·감독자 리뷰를 대신하지 않습니다. 최신 main의 .NET macOS accepted-request/stop 실패는 기존 #2981에서 추적하며, 이후 batch 통과로 결함이 해결됐다고 간주하지 않습니다.

=== zlink-systems 2026-10-28T21:03:43Z
머지 전 실행 스펙 대조에서 확인한 .NET·Node 요청 완료 결함을 같은 #2758 범위에서 수정·검증 중입니다. 아직 push/전체 gate/머지 승인을 하지 않았습니다.

- .NET canonical PREPARE가 실제 binding `TimedOut`을 `Unavailable`로 바꾸었습니다. 실제 native 수락 후 reply timeout(250ms, 바깥 요청15초)을 분리해 `DeadlineExceeded` 기대가 실패하는 것을 재현했고, 전용 분류를 없애 기존 named-request completion mapper를 재사용했습니다. 해당 회귀5회·원래 overlapping PREPARE5회·관련132건이 실패/skip0으로 통과했습니다. 공식 format 검사1250파일도 통과했습니다. 이전 혼합 실행의 overlap 실패는 당시 typed winner 기록이 없어 이번 별도 재현과 동일한 순서였다고 단정하지 않습니다.
- PREPARE 이후 source 복구는 matching Preserve 확인 전에 FIFO를 열던 경로를 고쳤습니다. Store 오류·다른 authority·실제 source 임대 만료 대조3건, 원래 복구/ordered-route7건의5회 반복을 확인했습니다. source authority를 확인할 수 없으면 닫힌 상태를 유지합니다.
- Node #2911은 내부 raw/relay 결과가 최종 Actor typed 결과보다 먼저 원래 entry를 완료하는 결함을 수정 중입니다. 기존 최종 수신자를 codec 소유 경로로 전달하고 Message Follow의 통지 제출·queue 회계를 유지합니다. 실제 remote prime, Actor68건, handoff73건과 통지/취소7건의 통과 로그를 확인했으나 전체 PR gate는 아직 아닙니다.

남은 .NET 작업은 canonical PREPARE의 별도 `CancelAfter`를 제거하고 readiness 이전부터 기존 operation entry 하나가 deadline과 최종 typed 수신자를 소유하도록 모으는 것입니다. `01-submit-and-completion.ko.md` §9·§10.1을 기준으로 진행하며 timeout 증가·자동 재제출·새 pending/table로 보상하지 않습니다. Node는 전체 diff 검토와 남은 relocation fixture 적응·통합 검증이 필요합니다.
=== zlink-systems 2026-10-29T10:08:15Z
대표님 지시(2026-10-29 09:57–09:58 UTC)에 따라 열린 원래 PR의 머지를 우선하고, 확대 검토·검증은 0.32.0 릴리즈 전에 다시 확인합니다. 새 Node 공개 receiver API 및 관련 보호 문서 변경은 이번 작업에서 제외하고 다음 버전 성능개선 때 재검토합니다. 이 이슈는 일부 PR 머지로 완료 처리하지 않습니다.

현재 실제 머지: #3064, #2830, #3142, #3021. #3021은 10:03:36 UTC, merge `e7c7e77703d3dac0919b352d31fa715d944760d6`이며 관련 Core 실제61건 PASS·실패/skip0입니다.

남은 원래 PR은 #2942, #2911, #2857, #2843, #2609입니다. 각각 main과 충돌하여 최소 통합을 병렬 준비합니다. 기존 미커밋 수정은 보존하고 실제 제출·머지되는 원격 커밋과 구분합니다.

릴리즈 전 확인할 실제 잔여:

- Node #2911 현재 통합 후보의 M6B는146개 중145개 통과·1개 실패입니다. test68의 stale Ready owner-fence Instance intent가 `reply` 기대에 `Unavailable`을 반환합니다. 동일 original request가 종결된 뒤 authority 재조회를 시도하는 경계와 네 언어의 admission 처리 차이를 확인해야 합니다. 시험 expectation 변경·재시도·새 pending/API로 숨기지 않습니다. 실제 로그와 읽기 전용 근거는 `D:/t/jobs/fx/cx/node2911-m6b-fixture-adapt-20261029` 및 `node2911-m6b-remaining-failure-20261029/report.txt`에 보존했습니다.
- Node M6C 최초323건은308통과·1실패·14취소였고, 실제 Closing 상태를 구성한 두 fixture 수정 뒤 전체 대상 파일15/15·4/4가 통과했습니다. 첫 컴파일의 unused import 실패와 정정 후 컴파일 통과도 보존합니다. 일반4125, M6A44, browser18, 환경 정정 interop13의 실제 통과와 구분합니다. 현재 통합 소스의 통과를 원래 원격 PR head의 검증으로 바꾸어 보고하지 않습니다.
- C++ 현재 미커밋 canonical 후보는 원래 PR 최소 통합과 별개입니다. prepared Actor overload에 맞춰야 할 기존 fake3개, 공통 self-Actor 거부 판정 위치, late physical result의 자원·cache invalidation 수명, trace 결과 분류·correlation·Off 비용/실행 중 수준 변경의 정합이 남아 있습니다. 현재 전체 라이브러리 빌드를 진행하지만 공식 동작 gate 통과로 보고하지 않습니다. 원래 WIP 및 외부 후보·원래 RED는 보존합니다.
- #2843은 기존 API로 permit 경계 이동이 가능하며 새 #2609 API를 기술적 선행 조건으로 두지 않습니다. 네 언어의 실제 callback 직전 반환과 실패 cleanup 경계는 릴리즈 최종 대조에 포함합니다.
- 새 공개 API 이연은 원래 #2609 전체 PR 변경을 폐기한다는 뜻이 아닙니다. 기존 계약 범위의 원래 PR 통합과 성능 목표 충족 여부를 구분합니다.

최종 변경 전체 리뷰와 아직 실행하지 않은 확대 검증은 완료로 기록하지 않습니다. 대표님이 지시하신 머지 우선 범위와 릴리즈 완료 판정은 구분합니다.

=== zlink-systems 2026-10-29T10:38:27Z
원래 열린 PR의 머지를 완료했습니다. 마지막 #2609는 2026-10-29 10:35:58 UTC, merge `676d7a3ce866610fbca4c65f70f5c973845f7439`입니다. GitHub 열린 PR 목록을 다시 조회해0개를 확인했습니다. 이번 작업 실제 머지9개는 #3064, #2830, #3142, #3021, #2942, #2911, #2857, #2843, #2609입니다.

대표님 지시대로 PR 머지 완료와 0.32.0 릴리즈 검증 완료를 구분합니다. Project README도 열린 PR0개로 갱신했습니다. 새 Node 공개 receiver API와 관련 네 보호 문서 변경은 이번 작업에서 제외했으며 다음 버전 성능개선 때 재검토합니다. Core 문서 네 곳은 승인된 공통 reply-first 계약 정리로 반영했습니다.

릴리즈 전 남는 항목은 다음과 같습니다.

- Node의 실제 M6B test68 실패(146개 중145통과, `reply` 기대에 `Unavailable`)는 해결됐다고 기록하지 않습니다. 원래 terminal과 Spot authority 재조회의 순서 및 네 언어 admission 경계를 대조해야 합니다. timeout·retry·assertion 변경으로 숨기지 않았습니다.
- Node 일반4125·M6A44·browser18·정정interop13 및 M6C 대상15/15·4/4의 통과는 보존한 해당 소스의 증거입니다. 이후 #2843·#2609 최종 통합 전체의 runtime 통과를 뜻하지 않습니다. 마지막7파일은 감독자가 전체 증분·실제 callee를 확인했고 JS6파일 구문검사만 통과했습니다.
- Node 최종 통합에서 기존 request-result class 기반 시험과 helper의 삭제·대체 범위를 전체 검토해야 합니다. 특히 `node-request-entry-lifetime.test.js`, `request-deadline-registration.test.js`, `request-result-budget.test.js`, completion-path/cost 시험 변경의 역할이 현재 original-operation·physical-receiver 시험으로 충분히 대체되는지 릴리즈 전에 확인합니다. 이를 검증 완료로 기록하지 않습니다.
- Node 기존 Promise 비용 RED와 최종 비용 목표 충족 여부는 별도입니다. 새 공개 API를 추가하지 않은 현재 코드의 최종 비용·수명 검증이 필요합니다.
- C++ 별도 미커밋 canonical 후보는 원래 PR 최소 통합에 포함되지 않았습니다. 원래 dirty worktree와 외부 diff·RED를 보존합니다. 그 후보의 `public_host_runtime.cpp:3308` 선언순서 컴파일 오류, Actor fake3개·self guard, late physical resource/cache invalidation, trace 분류·correlation·Off 비용과 실행 중 수준 변경은 남아 있습니다.
- C++ #2857의 삭제된 deadline helper 적응은 readiness 전용 scheduled registry를 연결했습니다. 같은 original operation 소유 계약, 취소·종료·자원 수명·비용을 최종 조합에서 다시 확인합니다. 이 적응에 새 runtime 검증을 수행하지 않았습니다.
- Core #3021 관련61개는 실패·skip0으로 통과했습니다. 기존 macOS accepted-request/stop 실패의 원래 조건에서 전체 해결됐는지는 #2981의 별도 릴리즈 확인으로 유지합니다.
- 원래 PR·커밋·최종 통합 범위 중 대표님이 머지 뒤로 넘기신 확대 검토와 플랫폼 gate는 완료로 바꾸지 않습니다. 열린 릴리즈 이슈도 PR 머지 개수만으로 닫지 않습니다.

완료된 clean 통합 worktree만 정리합니다. 원래 foreign WIP·미커밋 후보·원래 branch와 외부 검증자료는 보존합니다.

=== kairos-code-dev 2026-10-30T05:30:10Z
PM scope record for PR3201: the outer post-start physical-retention failure is reproduced (unchanged real Host.stop test PASS→one bad_alloc FAIL→exact restoration PASS). Root review: https://github.com/zlink-systems/zlink/pull/3201#issuecomment-6515520372 . Root approved existing invocation/drain-frame ownership with pending-only nonallocating inline executor credit; public head remains frozen until the complete correction is reviewed.

Additional SOURCE-ONLY candidate, not reproduced or included in the current correction approval: `framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/task.hpp:535–557` allocates a wait registration and may grow `_continuations` after an eager child task has started. `route_handler_registry.hpp:175–181`, `route_handler_invoker.cpp:24` and `runtime/handlers/handler_registry.cpp:104–139` can consequently observe await-link failure rather than the deepest child's physical terminal. Root read these source paths. A generic task linkage/intrusive-list redesign crosses the current four-handoff correction's scope and is not approved; retain this candidate here for the existing execution/lifetime audit. No use-after-free or cross-language OOM failure was observed.

The cross-language source packet identifies conditional post-invocation observation boundaries in .NET ApplicationSource/CompleteDispatchAsync, JVM HandlerDispatch.start/whenComplete (Kotlin uses that adapter path), and Node mesh-dispatch-pump finally/Set.add. These are diagnostic candidates subject to each runtime's actual exception semantics, not confirmed defects, a new allocation-error policy, or a claim that all languages need the same patch. Source packet: `/root/jobs/fx/cx/review-cpp-2757-2790-20261029/pr3201-cross-language-owner-evidence/cross-language-owners.md`.

Keep #2758 open. Neither this partial correction nor historical-default1.20 proof establishes general allocation-failure safety, matching official1.21 acceptance, or Framework0.31.0 release completion.

=== kairos-code-dev 2026-10-30T06:56:30Z
PM 진단 기록 (2026-10-30): C++ #3201과 같은 모양(handler 시작 뒤 terminal 관찰 등록 실패)의 다른 언어 후보를 런타임별로 확인했습니다. 진단은 codex sol 읽기 전용 job이고, 아래 두 위치는 PM이 main 8db67d6b73 소스를 직접 열어 확인했습니다.

| 언어 | 판정 | 근거 |
|---|---|---|
| Java | 결함 가능 — 조건 좁음 | `ZLinkHandlerDispatch.start`(ZLinkHandlerDispatch.java:47-59)의 한 `catch`가 `operation.get()` 실패(시작 전)와 `.whenComplete(...)` 등록 실패(시작 뒤)를 구분하지 않습니다. 등록이 던지면 `result`를 실패 완료해 `observe`가 claim을 닫는데, handler stage는 미완료일 수 있습니다. 등록 호출이 던지는 `CompletionStage` 하위형으로 국소 재현됐습니다. 기본 `CompletableFuture`에서는 OOM(`Error`)만 남습니다 |
| Node | 결함 가능 — 조건 좁음 | `observeRecordTerminal`(mesh-dispatch-pump.ts:571-573)에서 `activeDrains.add`가 V8 Set 용량 한계에서 `RangeError`를 던지면, finally는 연결됐지만 join Set에 없는 terminal이 생깁니다. 실제 pump가 그 규모에 이르는 Host 재현은 하지 않았습니다 |
| .NET | 확인 못 함 | `ValueTask` await 경로에서 OOM 외 등록 예외는 확인되지 않았습니다 |
| Kotlin | 확인 못 함 | Java claim 경로를 공유하지만 기본 adapter는 일반 `CompletableFuture`라 위 Java 재현 조건이 그대로 적용되지 않습니다 |

분류: 네 언어 모두 기본 설정의 통상 실행에서 확정된 결함이 아니며, C++ #3201과 같은 원인으로 판정하지 않습니다. Java·Node는 시작 뒤 등록 실패 시 물리 terminal을 누가 소유하는지 정해야 고칠 수 있습니다. 공통 할당 실패 정책이 필요해 이번 0.31.0 범위에 넣지 않고 이 epic에 후보로 둡니다.

원본: `D:/t/jobs/fx/cx/pm-2758-observers-diag-20261030/REPORT.md`, Java·Node 국소 재현 로그 `/tmp/pm-2758-observers-diag-20261030/{java,node}-test.log`(ulalax-home WSL).

=== kairos-code-dev 2026-10-30T07:20:18Z
PM 기록 (2026-10-30): source-only로 남겼던 C++ `task.hpp` continuation 등록 후보를 실제로 재현했습니다. 확정 결함이며 기존 결함(B)으로 분류합니다.

- 위치: `framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/task.hpp:535-557` `task_shared_state_t::set_continuation`. eager child가 이미 시작된 뒤 `make_shared`(:542)와 `_continuations.push_back`(:551)로 할당합니다.
- 재현: main 8db67d6b73, Debug, 로컬 패키지 1.20.0. 원본 :542 직전에 두 번째 등록에서 `std::bad_alloc`을 한 번 던지는 진단용 변경(커밋하지 않음, 원본 바이트 복원을 `cmp`로 확인)을 넣었습니다.
  - 기준선: `ManualDispatchOwner.ReplyFailureClosesPendingScopeAndPhysicalTerminalOnce` 1/1, `RouteHandlerBoundary.*` 6/6
  - 변경 후: 같은 manual dispatch 시험 0/1 — `test_cpp_framework_manual_dispatch_owner.cpp:718` scopes_destroyed 기대 0 실제 1, `:719` drain_until 기대 false 실제 true. 그 뒤 같은 child(state 0x…39950)가 성공 완료합니다. 부모 계열 task가 child 종료보다 먼저 실패 완료해 invocation scope와 executor drain 소유를 먼저 풀었습니다.
  - 원복: 7/7 통과
- PM이 raw 로그(`test-injected.log`의 DIAG 순서와 두 assertion 실패, `test-restored.log` PASS)를 직접 확인했습니다.
- 한계: 주입 stack은 classic handler registry 경로입니다. Route wrapper(`route_handler_registry.hpp:175`) 경로는 같은 코드를 지나지만 직접 주입하지는 않았습니다. use-after-free는 도구로 검증하지 않았습니다.
- 다음: 설계 대안 비교(읽기 전용 sol)를 시작했습니다. child 시작 뒤 결합을 비할당·비실패로 만드는 방향을 우선 보고, PM이 결정 소유 표로 A/B를 판정한 뒤 구현합니다.

원본: `D:/t/jobs/fx/cx/pm-2758-taskhpp-diag-20261030/` (REPORT.md, test-*.log, injection.diff, results.json).

