# Java request-backpressure 호출 대조

기준 commit은 ee7e5c3343이다. 이 작업은 성능 gate가 아니라 사용자가 지정한 실패 재현이다. L5~L7의 admission 전달 누락을 수정 전에 그대로 실행해 증거를 보존한다.

파일 경로 기준: client=`framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/`, server=`framework/bench/grpc/java/zlink-framework-server/src/main/java/systems/zlink/bench/withgrpc/frameworkserver/`, raw server=`framework/bench/grpc/java/zlink-raw-server/src/main/java/systems/zlink/bench/withgrpc/rawserver/`. Production 경로는 lad-java-impl5/call-shape-L5-7.md와 이 작업 report의 상세 경로를 따른다.

| 경로 | binding/L4 file:line | L5~L7 file:line | main Framework file:line | 동일 여부·차이 |
|---|---|---|---|---|
| source send | RawStack.java:162-191 (binding), :309-339 (L4) | FrameworkStack.java:177-182 | FrameworkStack.java:93-99 | L4 typed JSON과 public Framework node route, L7 operation/deadline 복원. Main은 protobuf 메시지다. |
| target receive | ZLinkRawBenchServer.java:94-109; L4 ZLinkFrameworkBenchServer.java:272-316 | ZLinkFrameworkBenchServer.java:468-474 → ZLinkMeshApplicationDispatcher.accept | ZLinkFrameworkBenchServer.java:475-482 → 같은 dispatcher | L5 owner queue/worker, L6 handler activation·scope. Binding/L4는 blocking recv 한 번 뒤 직접 handler. L5~L7/main은 production receiver/queue/worker. |
| target reply | ZLinkRawBenchServer.java:115-125; L4 ZLinkFrameworkBenchServer.java:291-337 | BenchTypedEchoHandler.java:13-27 → production reply encode/submit | BenchEchoHandler.java:13-27 → 같은 production reply | 모두 새 envelope와 새 body. L4/L5~L7 typed JSON, binding/main protobuf. L5~L7은 per-request scope·queue allocation/wait가 추가되며 bench metrics 소유자는 유지한다. |
| source admission·reply | RawStack.java:162-191, BenchDrivers.java:262-330 | FrameworkStack.java:183-191은 reply만 반환 | FrameworkStack.java:117-118은 admitted와 reply를 함께 반환 | **불일치**. L5~L7은 BenchOperation, main/binding은 BenchRequestOperation. BenchDrivers.java:610-621은 후자에서만 admitted를 기다린다. 이 누락은 복원 기능이 아니라 bench 결함 후보이며 실패 재현 대상이다. |

Binding과 L4도 같은 Core completion 한도를 적용받는다. raw 경로는 admission을 기다린 뒤 다음 request를 제출하므로 이 한도에 도달하는지는 실측/호출 형태로 구분한다. 고정 window 100은 request-backpressure에 적용하지 않는다.

## Bench 수정 후 대조

`FrameworkStack.java:177,191-192`는 `BenchRequestOperation`과 기존 `Submission(admitted, reply)`를 반환한다. Driver는 main과 같은 `BenchDrivers.java:610-621`에서 admission을 기다린다. Typed JSON payload, production owner queue·handler activation·operation registry·deadline·completion 경로는 그대로다. 추가 state·flag·queue·lock·timer·retry·고정 window는 없다.
