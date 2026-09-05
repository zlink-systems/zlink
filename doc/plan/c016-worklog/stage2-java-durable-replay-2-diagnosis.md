# Java durable replay 2차 진단

Java Framework의 기존 결함(B)을 수정하고 갱신된 sender 계약(A)에 적응한다. 아래 진단은
runtime 수정 전에 작성했다. 사용자 지시가 A/B 판정 뒤 Stage 2 구현을 명시적으로 승인했다.

- Owner: raw mesh의 durable request 경계가 encoded request, deadline, binding 결과의 phase와 admission 이력을 함께 소유한다. 연결 교체와 stranded request 완료는 Core, typed 결과 생성은 binding 소유다.
- Spec: actor-model §9의 sender replay bullets(:668–680), Core socket README §4(:160–169, bb730c654f). 동일 OperationId/header, terminal envelope 전 replay, attempt마다 전체 remaining deadline, never-admitted=Unavailable/admitted=DeadlineExceeded.
- Parity: C++ raw_route_port.hpp:51–70의 initial_admission/completion_terminal 구분을 Java의 원래 ZlinkSubmitException/ZlinkRequestException으로 유지한다. C++ raw_mesh_node_owner.cpp:159–327은 request_parts/correlation을 retry state가 소유한다. Node service-stateful-runtime.ts:4014–4053도 encoded header를 loop 밖에서 고정하고 typed admission 이력을 소비한다. C++ 현재 predicate의 계약 적응은 별도 작업이다.
- Class: A(갱신된 durable replay 계약 적응) + B(Java raw mesh의 typed phase 소실과 bind identity 재생성).

## 원인과 선택

Java runtime 경로는 `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/` 기준이다.

1. `binding/ZLinkJavaRawServicePort.java:173–184`는 원래 binding 예외를 보존한다.
   `binding/ZLinkJavaRawMeshNode.java:3612–3623`의 requestResult는 submit 실패를 INTERNAL_ERROR로
   축소한다. create의 :3489–3498/:3854–3858은 이를 원인 없는 IllegalStateException으로,
   bind의 :2910–2920은 새 request 예외로 바꾼다. bind preflight(:2873–2880)의 synthetic
   request NOT_CONNECTED도 admission 증거가 아니다.
2. 공용 requestResult의 반환형을 새 phase DTO로 바꾸면 application/User Spot 등 요청 범위 밖
   호출자까지 전파된다. 대신 durable 전용 내부 request 경계에서 원래 Throwable을 소비한다.
   submit 예외는 미수락, request 예외는 수락 뒤 completion이다. errno는 읽거나 재분류하지 않는다.
   실제 request NOT_CONNECTED는 handover marker로 replay하며 admission 이력은 유지한다.
   preflight는 아직 제출하지 않은 상태로 기다리고 synthetic request 예외를 만들지 않는다.
3. `actors/ZLinkBoundSessionRuntime.java:168–173`의 retry는 매번 stream submit을 호출한다.
   `binding/ZLinkJavaStreamSocket.java:337–355`는 generation을, raw mesh :2889–2901은
   correlation/header를 다시 만든다. 상위 loop 유지 + identity 전달 API 추가는 지식을 두 계층에
   나눈다. 이를 채택하지 않고 raw mesh 안에서 header를 한 번 생성한 뒤 반복 제출한다.
   상위는 bind operation을 한 번 submit하고 전체 timeout을 전달한다. route preflight가 끝나면
   첫 header를 고정하며 이후 replay에서도 그대로 쓴다.
4. Join(:1373–1418)과 create(:3410–3500)도 같은 내부 replay 경계를 사용한다. 수신한 envelope의
   decode/terminal 처리는 loop 밖에 둔다. create의 별도 operation registry timeout이 typed
   completion보다 먼저 원인 없는 timeout으로 끝내는 경쟁도 제거하고 durable request 완료를
   decode로 직접 연결한다. 일반 requestResult 호출자는 유지한다.
5. `actors/ZLinkActorSpotJoinCall.java:701–715`의 topology 기반 timeout 재분류를 제거한다.
   create target 대기, Join admission 전 gate, bound route 대기의 소진은 Unavailable이다.
   admission 뒤 소진은 durable sender가 typed request 결과로 DeadlineExceeded를 결정한다.

## 구현·검증 경계

Java runtime와 관련 core test만 수정한다. 3개 operation에 route 부재, admitted reply 유실,
NOT_CONNECTED handover 뒤 성공을 검증하고 동일 encoded identity/remaining budget/terminal 뒤
중단을 확인한다. 재현 로그는 보존한다. touched tests → core test 1회 → TicTacToe/GameQuest
각 1회 순서로 실행하며 모든 Gradle 실행은 `/tmp/zlink-jvm-gate.lock`을 `flock -w7200`으로
보호한다. 09:49 Java package(c9d294c44f)를 재사용하고 rebuild하지 않는다.

Core의 handover 즉시 NOT_CONNECTED 구현은 별도 작업 중이다. deterministic typed-result
회귀는 그 계약을 입력으로 검증하며 native handover가 옛 timeout 동작이면 공개 API 재현과
함께 Core blocker로 구분한다. Assertion·deadline·spec 변경과 commit은 하지 않는다.
