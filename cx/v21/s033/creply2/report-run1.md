검증 진행 중인 초안이다. C++ 빌드·경쟁 테스트·전체 CTest·성능 측정의 완료 보고가 아니다.

`raw_route_port_t::reply()`는 기존 socket mutex 안에서 `_socket == nullptr`를 한 번 확인하고 binding operation을 구성한다. Operation이 native handle과 weak runtime 참조를 보관한 뒤 mutex를 풀고 `.submit()`을 호출한다. Binding terminal은 runtime의 strong 참조를 얻으며 C++ socket 객체를 다시 읽지 않는다. Native admission과 close 가능 여부는 Core가 판정한다.

종료 측은 port를 먼저 닫아 새 operation 생성을 막는다. `close_route_socket()` 한 곳에서 binding의 명시적 `close()`를 호출한다. BUSY일 때만 1 ms 뒤에 재시도하며 Java PM 변경과 같은 총 8회 시도, 최대 7회 대기다. 8번째 BUSY와 다른 close 오류는 그대로 전달한다. `unique_ptr`는 성공한 close 뒤에만 비우므로 기존 owner와 failure collector가 실패한 socket을 보존한다. Runtime의 새 queue·lock·cache·counter·flag·member state는 없다. 재시도 횟수는 해당 close 호출의 지역 변수다.

검토한 대안은 port 자체가 binding socket까지 닫는 방식과 socket owner가 닫는 방식이다. `raw_route_port_t`는 외부 socket도 빌리므로 owner에서 닫는 방식을 선택했다. Borrowed socket의 소유권을 바꾸지 않는다.

아래 Framework 경로는 `framework/languages/cpp/framework/src/` 기준이다. 줄 번호는 최종 소스에서 다시 확인할 예정이다.

| 종료 위치 | Unlock 뒤 진행 중인 reply와 겹칠 수 있는가 | 안전성 |
|---|---|---|
| `runtime/backend/raw_route_port.cpp`, `raw_route_port_t::close` | 그렇다 | 기존 mutex로 poller를 분리하고 `_socket`을 비운다. Binding socket을 소유하거나 파괴하지 않는다. 이후 reply는 기존 null 검사에서 거절된다. |
| `runtime/backend/raw_route_port.cpp`, `close_route_socket`의 explicit close와 reset | 그렇다 | 실제 Core close 결과가 BUSY면 한도 안에서 재시도한다. 성공 뒤에만 wrapper를 소멸시킨다. 실패하면 포인터와 원래 예외를 보존한다. |
| `runtime/mesh/raw_mesh_node_owner.cpp`, `close()`의 자원 tuple 정리 | 그렇다 | `close_mesh_transport`가 port를 먼저 닫고 poller와 router를 정리한다. 실패한 자원은 owner에 되돌린다. |
| 같은 파일, owner destructor와 retained cleanup closure | 그렇다 | Destructor가 같은 close를 실행한다. 실패하면 context·mutex·port·poller·router를 기존 failure collector에 함께 넘기며 retained cleanup도 같은 정리 함수를 사용한다. |
| 같은 파일, `start()`의 local router 소멸 및 `_router` 대입 | 정상 start 수명에서는 아니다 | Local router는 start 성공 전 자원이다. Close가 자원을 꺼낸 동안 start가 새 router를 만들지 못하도록 기존 `_closed` 설정을 close의 owner turn으로 옮겼다. 실패한 close 뒤에도 소켓을 덮어쓰지 않는다. |
| `runtime/client_server/raw_client_server_owner.cpp`, server `close()`와 공통 자원 정리 | 그렇다 | Port와 monitor를 먼저 분리한 뒤 router에 공통 close 재시도를 적용한다. 실패한 router·monitor를 원래 owner로 되돌린다. Dealer 정리는 기존 경로를 사용한다. |
| 같은 파일, server destructor와 retained cleanup closure | 그렇다 | 같은 자원 정리 함수를 사용한다. 실패한 자원과 context·mutex를 기존 collector에 보존한다. |
| 같은 파일, server `start()`의 local router 소멸 및 `_router` 대입 | 정상 start 수명에서는 아니다 | Start 완료 전에 구성하는 router다. 실행 중에는 기존 `_port` 검사, 성공한 close 뒤에는 기존 `_closed` 검사가 교체를 막는다. |
| 같은 파일, `start(router_socket_t&)` | 외부 owner에 달렸다 | Framework가 binding wrapper를 소유하지 않는다. 외부 owner가 port close 뒤 명시적 socket close와 실패 처리를 완료해야 한다. 경쟁 fixture는 이 순서로 공통 close 함수를 사용한다. |
| `bindings/cpp/src/Runtime/Sockets/socket.cpp:53`, `socket_t::~socket_t`와 `Runtime/Native/socket_handle.hpp`의 destructor | 보호 없이 소멸시키면 겹친다 | 두 소멸자는 native close 실패를 전달하지 않는다. Prototype 대조군은 이 경로에서 BUSY를 무시했을 때 native handle이 남는지를 검사한다. Production owner는 이미 명시적 close 실패를 보존하며 이번 변경은 그 경로에 재시도를 추가한다. |
| `bindings/cpp/src/Runtime/Sockets/socket.cpp:62`, socket move assignment | Framework의 실제 경로에서는 호출하지 않는다 | Framework는 `unique_ptr`를 이동하며 binding socket 객체 자체를 move-assign하지 않는다. Mesh의 close 중 재시작도 기존 상태로 거절한다. |

Close가 operation 구성보다 먼저 mutex를 얻으면 reply는 false를 반환한다. Operation 구성 뒤 native 진입 전에 close가 끝나면 binding runtime 또는 Core handle gate가 제출을 거절한다. Native admission이 먼저 수락되면 Core가 BUSY 또는 안전한 accepted close를 판정한다. 성공한 native close 뒤 wrapper가 소멸해도 제출 operation은 wrapper를 다시 읽지 않는다. 테스트는 Context를 유지한 상태의 socket close 경합을 대상으로 한다.

소유 계층은 Framework의 C++ wrapper 접근·자원 소유권과 binding/Core의 native admission·close 판정이다.

Spec 근거는 `bindings/doc/spec/README.ko.md`의 Multipart 제출과 별도 송신 gate 금지, `core/doc/spec/core/socket/README.ko.md` §2의 same-handle concurrency·close gate다. 종료 재시도 정책은 사용자에게 제시된 Java PM 결정의 총 8회·1 ms 정책을 적용한다.

교차언어 대조는 C++ port mutex, Java 원본의 state lane 및 `/root/jobs/fx/cx/v21/s033/jdr4/change.diff`의 직접 reply·close 재시도, .NET `ZLinkRawRouterServicePort.cs:247`과 Node `node-raw-binding-port.ts:260`의 직접 binding 제출이다. 실행 검증 범위는 요청된 C++ unit·contract suite다.

변경 분류는 A, 기존 binding 동시 제출 계약의 적용과 승인된 종료 정책 적용이다. Mesh의 기존 close 상태 설정 시점과 실패 수집 중복 정리는 B, 기존 lifecycle 결함 및 내부 중복 수정이다.

<!-- TEST_RESULTS -->
기존 mutex 계측기는 재검사를 통과했다. `logs/mutex-validation.log`에 정상 100회·의도한 경합 1회·범위 밖 제외 결과를 기록했다.

기존 close-failure 검사 35개는 `logs/focused.log`에서 모두 통과했다. 새 reply/close 검사 100회와 결정적 검사 1회는 `logs/focused-retry2.log`, 각 native 결과는 `logs/focused-LastTest.log`에 기록했다. 101회 모두 실제 Core BUSY 1회 뒤 두 번째 close가 성공했다. 종료 뒤 같은 native handle의 직접 close는 모두 SHUTDOWN(402)이었다. 재시도 간격은 최소 1.087701 ms, 중앙값 1.101197 ms, 최대 5.535186 ms였다. 8회 BUSY 한도와 소유권 보존, BUSY 외 오류의 즉시 전달도 통과했다.

`logs/prototype-destructor-control.log`의 대조군은 원래 `prototype.diff`만 적용한 backend를 별도로 컴파일해 실행했다. 100회 모두 binding wrapper의 두 소멸자 close가 실제 BUSY를 반환했고, wrapper 소멸 뒤 native handle의 close가 OK(0)였다. Native socket이 소멸자에서 닫히지 않았음을 확인한 뒤 이 직접 close로 정리했다. 대조군 종료 코드 1은 의도한 실패다. `close-validation.json`, `negative-control.json`, `prototype-control-source.json`에 결과와 입력을 기록했다.

처음 작성한 대기 helper의 predicate 중복 호출과 native 정지 시점 때문에 발생한 fixture 실패 로그는 `logs/focused-initial-LastTest.log`, `logs/focused-timeout-LastTest.log`에 보존했다. `logs/diagnose-hold-stack.log`에서 poller 해제와 native send가 같은 Core control lock을 기다리는 것을 확인했다. Fixture는 operation이 이미 submit에 들어간 상태에서 port를 먼저 논리 종료하고 native 진입을 허용해, 포트 종료와 실제 native close 사이의 허용된 경합을 재현한다. Runtime 정리 순서와 검사의 시간 한도·assertion은 완화하지 않았다.

전체 C++ unit·contract 299개를 위한 target 165개를 빌드 중이다. 전체 suite 결과는 아직 완료되지 않았다.

<!-- MEASUREMENT_RESULTS -->
RUNS=5 및 동일 40k req/s 측정은 아직 실행하지 않았다. 처리율 역수의 µs/request를 사용하며 budget은 binding µs/request ÷ 0.6으로 계산할 예정이다. 대기 중인 빌드 때문에 성능 개선 수치를 주장하지 않는다.

<!-- REFACTOR_RESULTS -->
Native reply 제출 직렬화의 판정 위치는 Framework mutex와 Core의 2곳에서 Core 1곳으로 줄였다. C++ wrapper와 close-start null 검사의 판정 위치는 기존 mutex 1곳이다. Native close 수락 판정은 Core 1곳이며 Framework의 BUSY 재시도 정책은 공통 함수 1곳에만 둔다. Mesh 재시작 허용 판정과 기존 `_closed` 상태의 수는 각각 1곳과 1개로 유지한다.

중복 작업 점검의 최종 file:line과 변경 파일·검증 결과는 실행 완료 뒤 채울 예정이다.
