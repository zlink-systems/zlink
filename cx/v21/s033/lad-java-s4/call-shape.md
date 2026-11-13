# Java L4·L5~L7 측정 호출 대조

| 경로 | Binding bench (file:line) | L4 (file:line) | L5~L7 (file:line) | 동일 여부·추가 비용 |
|---|---|---|---|---|
| Source send | `RawStack.java:309-339` (typed L4 baseline; binding submit `:162-191`) | `RawStack.java:309-339` → `FrameworkStack.java:163-166` | `FrameworkStack.java:177-192` | L4는 typed JSON 경로. L5~L7은 operation 등록과 기존 admission future 전달을 포함. 고정 크기 window 추가 없음. |
| Target receive | `ZLinkRawBenchServer.java:94-109` | `ZLinkFrameworkBenchServer.java:272-316` | `ZLinkFrameworkBenchServer.java:419-435` → production dispatcher | Binding/L4는 메시지당 blocking receive 1회. L5~L7은 owner queue·worker 수신으로 queue wait와 관련 할당을 복원. |
| Target reply | `ZLinkRawBenchServer.java:115-127` | `ZLinkFrameworkBenchServer.java:291-337` | `BenchTypedEchoHandler.java:13-27` → `ZLinkMeshApplicationDispatcher.java:674-701,795-805` | 모두 새 reply envelope/body를 만들고 두 part를 제출. L4/L5~L7은 typed JSON encode; L5~L7은 production reply 경로의 queue/activation 작업을 포함. |
| Source reply completion | `RawStack.java:309-339` | `RawStack.java:309-339` → `FrameworkStack.java:163-166` | `FrameworkStack.java:183-192`; 기존 driver `BenchDrivers.java:610-621` | L4는 typed reply decode. L5~L7은 operation completion·deadline·dispatcher를 복원하고 admission을 기존 driver에 전달해 같은-stream backpressure를 유지. |

Binding/L4 receive는 메시지당 blocking wait 1회, L5~L7에는 owner queue/worker 대기와 Framework 할당이 추가된다. 세 경로는 bench metrics 소유자와 카운터를 유지하며 별도 metrics atomic, 추가 복사, 두 번째 poller를 넣지 않는다. L4/L5~L7은 typed JSON payload를 새 응답으로 직렬화하므로 요청 part 재사용이 없다. 차이는 ladder-steps.md가 정한 Framework 복원 기능에 한정된다. L5~L7 admission 연결은 `cffa8009d3`에서 수정된 bench 경로다.
