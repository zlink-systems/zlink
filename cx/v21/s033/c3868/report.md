# #3868 C++ 송신 경로 할당·잠금 절감

진행 중이다. 브랜치·시작 commit은 [head.txt](raw/head.txt), 포함된 변경은 [history.txt](raw/history.txt)에 보존했다. 제품 수정과 로컬 commit만 수행하며 push하지 않는다.

## Step 0: handler 수명과 결정 소유

| 항목 | 판정 | 근거 |
|---|---|---|
| 벤치 handler | route mesh Node의 route send handler, object role none | `framework/bench/grpc/cpp/framework/bench_framework_cpp_server.cpp:31,66–70` |
| 적용 수명 | Channel/route dispatch마다 새 scope·handler. 공유·재사용 금지 | API §10–11 `00-foundation/06-framework-api.ko.md:490–523` |
| 최적화 허용 범위 | 등록 시 생성 절차 준비, service key 직접 조회, dispatch 고유 저장소 | S08 `01-execution/08-messaging-hot-path.ko.md:253–265` |
| C++ 현재 경로 | route invoker가 새 scope 생성, 등록된 typed invoker가 새 handler 생성 | `route_handler_invoker.cpp:38–53`, `configuration/mesh_node.hpp:555–615`, `services.hpp:321–325` |
| Spot/Actor 결함 여부 | 이 벤치 경로에서 Spot/Actor instance를 dispatch마다 생성하지 않음. Node record는 공용 mesh dispatcher로 전달됨 | `mesh_record_dispatcher.cpp:38–80`, `spot_runtime.cpp:13624–13628`의 Spot scope는 activation 소유 |

아래 경로는 각 언어의 `framework/languages/<언어>/` 기준이다. 코드에서 보이는 생성만 기술하며 런타임 할당 횟수로 환산하지 않는다.

| 언어 | 같은 route handler의 dispatch별 생성·조회 | 위치 |
|---|---|---|
| .NET | 새 AsyncServiceScope와 handler. 등록된 DispatchCatalog의 factory로 dependency 조회. filter 없음 경로에는 filter owner/list 생성 없음. 비동기 실행 상태와 context 생성 | `src/Zlink.Framework/Runtime/Handlers/ZLinkHandlerDispatcher.cs:96–108`; `Runtime/Channels/ZLinkRouteHandlerInvoker.cs:69–98`; `ZLinkScopedHandlerInstanceOwner.cs:97` |
| Java | 새 handler와 dispatch 고유 dependency 수명. Scoped dependency가 없으면 빈 activation wrapper는 재사용 가능. 사전 준비된 constructor/서비스 key 사용. filter 목록은 등록 상태 재사용. filter 없음 경로에는 owner map 생성 없음. 필요한 activation wrapper·handler·완료 continuation 생성 | `zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelHandlerInvoker.java:435–460`; `runtime/internal/handlers/ZLinkHandlerActivator.java:69–90,500–520` |
| Node | 사전 준비된 activate가 새 handler 생성. DI 필요 시 scope factory 사용, 기본 준비 경로는 빈 scope 객체 생략. filter list는 등록 시 준비. 일반 경로에는 새 scope·Map·owned array·Promise 생성 | `packages/framework/src/runtime/channels/channel-dispatch-services.ts:131–150,195–212`; `runtime/handlers/handler-instance-scope.ts:45–57,76–86,132–156` |
| C++ | 새 shared scope state·handler, singleton service key 조회. 등록 filter list 재사용. scope state·handler control block·소유 vector·coroutine·type-erased callable의 heap 할당이 코드에 보임 | `framework/src/runtime/configuration/services.cpp:273–274,320–328`; `framework/include/zlink/framework/contracts/configuration/services.hpp:285–325`; `route_handler_invoker.cpp:38–53`; `handlers/handler_registry.cpp:349–357` |

| 결정 | spec 소유 | 현재 판정 위치(언어별) | 단일 소유자·변경 분류 |
|---|---|---|---|
| scope·handler 생성·정리 | API §10–11, S08 §4 | 위 언어별 표의 dispatcher·activator | Framework DI scope·등록 factory. B: 기존 절차의 저장·호출 비용 절감 |
| 최신 peer descriptor 조회 | MeshNode topology·liveness, 제어 결정 소유 원칙 | C++ `service_topology_registry.cpp:248–278,494–521`; Java `ZLinkJavaRawMeshNode.java:206–244,1777,1800`; .NET `Runtime/Service/ZLinkManagedMeshNode.cs:10890–10897`; Node `runtime/foundation/raw-service-mesh-runtime.ts:391,1821` | Framework topology owner가 immutable publication 게시. 등록 시 peer 복사 금지. B |
| serializer 선택 | codec registry가 content type·typed serializer 소유 | C++ `serializer.cpp:115–119,164–219,232–235`; 각 언어 handler invoker는 등록 codec 사용 | Framework codec registry. 등록은 startup에 동결되지만 typed resolved cache는 lazy 생성·게시됨. 별도 cache 추가 없이 기존 publication 교체. B |

Java의 `peerObservation`은 owner가 immutable map을 만들고 volatile root 하나로 게시한다(`ZLinkJavaRawMeshNode.java:206–244,565–576`). C++도 이미 단일 immutable topology가 있으므로 별도 descriptor 상태를 추가하지 않는다. 구형 shared_ptr atomic API의 내부 pthread mutex를 제거하는 것으로 범위를 좁힌다. Core·binding의 물리 admission·retry와 execution gate는 수정하지 않는다.

위 spec·생성·게시 경로의 줄 번호와 원문은 [Step 0 근거](raw/step0-source-evidence.txt)에 보존했다. C++20 atomic shared_ptr의 구현은 내부 spin lock을 사용하므로 엄밀한 lock-free로 표기하지 않는다. 이 대안을 허용할지는 사용자에게 확인 중이며, 답변 전에는 Step 2를 구현하지 않는다.

## Step 1: dispatch 생성 비용

적용 diff는 [step1.diff](raw/step1.diff)다. Scope와 handler의 생성·종료 지점은 유지했다. Scope 소유 vector의 첫 handle을 scope 내부 메모리에 보관하며, dependency가 많아지면 같은 vector의 기존 성장 절차를 따른다. Frame이 소유한 handler 호출자를 filter pipeline에 빌려주어 큰 closure를 복사하지 않는다. 송신 완료 closure를 값 타입으로 옮기고 사용하지 않는 reply callable 복사를 제거했다. Mesh dispatcher의 임시 channel 문자열도 이동한다.

대안은 인스턴스 저장소를 직접 구현하는 방법과 표준 PMR vector에 scope 고유 inline 메모리를 주는 방법이었다. 후자를 택해 별도 저장소 helper·성장 규칙을 추가하지 않았다. Scope state와 handler 자체의 shared allocation은 유지한다. `decode_application_parts`의 shared backing과 external part 수명도 유지한다.

현재 측정은 변경 전까지 완료됐고 변경판 빌드가 대기 중이다. [count capture](raw/step0/counts-zlink-framework-cpp/run1/zlink-framework-cpp-send-saturation-1024/capture/bpf.txt), [site capture](raw/step0-resume/sites-zlink-framework-cpp/run1/zlink-framework-cpp-send-saturation-1024/capture/bpf.txt), [집계](raw/capture-summary.json)에 원본과 검산을 보존했다.

수집기 연결 오류로 중단된 site 수집은 성능 결과에 포함하지 않는다. [실패 로그](raw/step0/sites-zlink-framework-cpp/run1/zlink-framework-cpp-send-saturation-1024/runner.log)와 원본은 보존했다. 재수집에서 count와 site를 같은 probe에서 세며, 빈 stack을 미분류 항목으로 포함해 총계 누락을 막았다. Stack 심볼 해석 실패는 수량 누락과 구분한다.

## Step 2·3: snapshot 읽기 방식 확인 대기

실제 적용하지 않은 최소 변경안은 [proposed-snapshot.diff](raw/proposed-snapshot.diff)다. 기존 immutable root·소유자·게시 절차를 그대로 쓰고 shared_ptr publication을 C++20 atomic specialization으로 바꾸는 안이다. 다만 배포 libstdc++의 `atomic<shared_ptr>::load`는 내부 spin lock을 사용한다. 이는 사용자 조건인 “read it without a lock”을 엄밀하게 충족하지 않으므로 허용 여부를 확인한 뒤 적용해야 한다. 구현 근거는 [stdlib 원문](raw/step0-source-evidence.txt)에 보존했다.

| 대안 | 보장 | 조건과의 관계 |
|---|---|---|
| C++20 atomic shared_ptr | 안전한 snapshot 소유·회수, pthread mutex 제거 | 내부 spin lock 허용 확인 필요 |
| raw atomic pointer와 hazard/epoch 회수 | 엄밀한 lock-free 읽기 가능 | 별도 회수 상태·reader counter가 필요해 새 flag·counter 금지 조건 변경 필요 |
| raw pointer와 모든 이전 root 보존 | 회수 경쟁 없음 | 갱신마다 메모리가 누적되므로 제외 |

Serializer 등록은 `serializer.cpp:115–119`에서 frozen 이후 변경을 거부하고, `app.cpp:3417`에서 시작 시 동결한다. Target handler codec은 `mesh_node_host_service.cpp:2674`에서 한 번 bind한다. 반면 source의 임의 typed send는 `channel.hpp:835–838`에서 `get<T>()`를 호출하며, resolved cache는 첫 type 사용 때 `serializer.cpp:177–196`에서 계속 게시된다. 따라서 등록 내용의 불변성과 resolved cache의 불변성을 구분해야 한다. 이 cache에도 별도 복사본 없이 같은 publication 변경안을 적용할 수 있다. 근거는 [Step 0 원문](raw/step0-source-evidence.txt)이다.
