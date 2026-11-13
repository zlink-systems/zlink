# Java binding 대 ladder 호출 형태

| 단계 | 방향 | binding bench 호출 | ladder bench 호출 | 동일 여부 / 복원 기능 |
|---|---|---|---|---|
| L0 | source send | `RawStack.java:75-98` request / raw send; `:243-274` payload·두 part 제출 | `RawStack.java:75-98,243-274`; L0 선택 `FrameworkStack.java:132-143` | 동일. 고정 handler 직접 호출만 복원 |
| L0 | target receive | `ZLinkRawBenchServer.java:94-109` blocking `recv(NONE)` | `ZLinkFrameworkBenchServer.java:151-177` blocking `recv(NONE)` 및 L0 handler | 수신 방식 동일; 고정 handler 직접 호출 |
| L0 | target reply | `ZLinkRawBenchServer.java:115-127` 새 protobuf reply·2 part submit | `ZLinkFrameworkBenchServer.java:211-243` 새 protobuf reply·2 part submit | 동일 |
| L0 | source completion | `RawStack.java:90-98` reply completion·검증 | `RawStack.java:90-98` | 동일 |
| L1 | source send | `RawStack.java:75-98,243-274` | 동일 | 동일; receive-side permit만 L1 복원 |
| L1 | target receive | `ZLinkRawBenchServer.java:94-109` | `ZLinkFrameworkBenchServer.java:136-177` | permit 획득/반납 추가 (application job queue admission) |
| L1 | target reply | `ZLinkRawBenchServer.java:115-127` | `ZLinkFrameworkBenchServer.java:211-243` | 동일 protobuf reply |
| L1 | source completion | `RawStack.java:90-98` | `RawStack.java:90-98` | 동일 |
| L2 | source send | `RawStack.java:243-274` | `RawStack.java:259-290`; selection `FrameworkStack.java:146-162` | request envelope encode 추가 |
| L2 | target receive | `ZLinkRawBenchServer.java:94-109` | `ZLinkFrameworkBenchServer.java:211-237` | envelope header parse 1회 추가 |
| L2 | target reply | `ZLinkRawBenchServer.java:115-127` | `ZLinkFrameworkBenchServer.java:249-269` | 새 response envelope + protobuf body, 2 part submit |
| L2 | source completion | `RawStack.java:90-98` | `RawStack.java:404-433` | response envelope parse 1회 추가 |
| L3 | source send | `RawStack.java:259-290` | `RawStack.java:280-318` | service-wire multipart frame 추가 |
| L3 | target receive | `ZLinkRawBenchServer.java:94-109` | `ZLinkFrameworkBenchServer.java:351-419` | service header/frame unpack 추가; blocking recv 유지 |
| L3 | target reply | `ZLinkRawBenchServer.java:115-127` | `ZLinkFrameworkBenchServer.java:400-419` | 새 service reply frame·2 part submit |
| L3 | source completion | `RawStack.java:90-98` | `RawStack.java:151-180` | service reply frame unpack 추가 |
| L4 | source send | `RawStack.java:280-318` | `RawStack.java:309-339,402-413` | typed JSON encode 추가 |
| L4 | target receive | `ZLinkRawBenchServer.java:94-109` | `ZLinkFrameworkBenchServer.java:272-316` | typed payload decode 추가 |
| L4 | target reply | `ZLinkRawBenchServer.java:115-127` | `ZLinkFrameworkBenchServer.java:291-337` | typed JSON reply 새 encode·frame·submit |
| L4 | source completion | `RawStack.java:151-180` | `RawStack.java:415-428` | typed JSON reply decode 추가 |
| L5~L7 | source send | `RawStack.java:309-339` | `FrameworkStack.java:167-174,195-197` | public route submission; L5~L7 owner/operation 경로 |
| L5~L7 | target receive | `ZLinkRawBenchServer.java:94-109` | `ZLinkFrameworkBenchServer.java:419-435` handler 등록 후 production dispatcher | L5 owner queue/worker |
| L5~L7 | target reply | `ZLinkRawBenchServer.java:115-127` | `BenchTypedEchoHandler.java:13-27`; production `ZLinkMeshApplicationDispatcher.java:674-701,795-805` | L6 activation/DI 및 typed reply 경로 |
| L5~L7 | source completion | `RawStack.java:151-180` | `FrameworkStack.java:177-192` | L7 operation registry/deadline/completion |

L0~L4 source·target마다 고정 receive wait는 메시지당 blocking recv 1회다. L1은 permit 할당·획득·반납을 추가한다. L2~L4는 각 단계의 envelope/frame/typed codec 할당·복사 비용을 추가하지만 별도 metrics atomic은 추가하지 않는다. L0~L4 응답은 매번 새 response part를 인코딩해 두 part를 submit하며 request parts를 재사용하지 않는다. L5~L7은 Framework route 경로로 queue/worker, activation/DI, operation completion을 복원한다.
