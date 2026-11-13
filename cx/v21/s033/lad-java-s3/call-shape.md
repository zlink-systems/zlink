# Java L4·L5~L7 호출 형태

| 단계 | 경로 | Binding | Framework ladder | 같은 점·차이 |
|---|---|---|---|---|
| L4 | source send / reply completion | `RawStack.java:309-339,402-428` | 동일 typed JSON 경로, `FrameworkStack.java:163-166` 선택 | 송신·완료는 같은 경로. typed JSON encode/decode 비용 포함 |
| L4 | target receive / reply | `ZLinkRawBenchServer.java:94-109,115-127` | `ZLinkFrameworkBenchServer.java:272-337` | 메시지마다 같은 blocking receive와 metrics atomic. JSON decode 후 새 reply를 encode해 두 part 제출 |
| L5~L7 | source send / reply completion | L4 `RawStack.java:309-339,151-180` | `FrameworkStack.java:167-198` | operation 등록, route submission, completion을 복원 |
| L5~L7 | target receive / reply | `ZLinkRawBenchServer.java:94-109,115-127` | `ZLinkFrameworkBenchServer.java:419-435`, `BenchTypedEchoHandler.java:13-27`, `ZLinkMeshApplicationDispatcher.java:674-701,795-805` | owner queue/worker, handler activation·DI scope, typed reply production route를 복원 |

L4는 기본 JSON typed serializer를 사용한다. L5~L7은 protobuf codec을 등록하지 않으며 typed DTO도 JSON 경로를 쓴다. Binding과 ladder의 raw receive는 메시지당 blocking receive 1회다. L5~L7에는 Framework dispatch queue·worker·completion 대기와 할당이 추가된다. 양 단계 모두 새 typed reply를 만들어 두 part로 제출한다. 상세 대조는 `/root/jobs/fx/cx/v21/s033/lad-java-s2/call-shape.md`와 ladder 단계 정의를 참조했다.
