# Java binding과 ladder 호출 형태

| 단계 | 경로 | binding bench | ladder bench | 같은 점과 복원 기능 |
|---|---|---|---|---|
| L0 | source send | `RawStack.java:75-98,243-274` | 동일 호출, 선택 `FrameworkStack.java:132-143` | send·completion은 동일. target의 고정 handler 직접 호출만 Framework로 복원 |
| L0 | target receive / reply | `ZLinkRawBenchServer.java:94-109,115-127` | `ZLinkFrameworkBenchServer.java:151-177,211-243` | 메시지마다 blocking `recv(NONE)` 한 번, protobuf reply 새 인코딩 후 2 part 제출. 고정 handler 호출 외 추가 차이 없음 |
| L1 | target receive | `ZLinkRawBenchServer.java:94-109` | `ZLinkFrameworkBenchServer.java:136-177` | L0에 application job queue permit 할당·획득·반납 추가 |
| L2 | source send / completion | `RawStack.java:259-290,404-433` | 동일 stack, `FrameworkStack.java:146-162` 선택 | 요청·응답 envelope encode 및 header parse 1회 추가 |
| L2 | target receive / reply | `ZLinkRawBenchServer.java:94-109,115-127` | `ZLinkFrameworkBenchServer.java:211-237,249-269` | header parse 1회, 새 response envelope·protobuf body encode 후 2 part 제출 |
| L3 | source send / completion | `RawStack.java:280-318,151-180` | 같은 stack의 L3 branch | service-wire multipart framing pack/unpack 추가 |
| L3 | target receive / reply | `ZLinkRawBenchServer.java:94-109,115-127` | `ZLinkFrameworkBenchServer.java:351-419` | blocking receive 유지, service header/frame unpack 및 새 framed reply 추가 |
| L4 | source send / completion | `RawStack.java:309-339,402-428` | same L4 typed payload path | typed JSON encode/decode 추가 |
| L4 | target receive / reply | `ZLinkRawBenchServer.java:94-109,115-127` | `ZLinkFrameworkBenchServer.java:272-337` | typed payload decode·새 reply encode 추가 |
| L5~L7 | source send / completion | L4 `RawStack.java:309-339,151-180` | `FrameworkStack.java:167-198` | public route submission과 operation registry/deadline/completion 복원 |
| L5~L7 | target receive / reply | `ZLinkRawBenchServer.java:94-109,115-127` | `ZLinkFrameworkBenchServer.java:419-435`, `BenchTypedEchoHandler.java:13-27` | owner queue/worker, handler activation·DI scope, typed reply production route 복원 |

공통 비용 비교: L0~L4는 binding과 동일하게 source completion wait, target 메시지당 blocking receive 1회, 같은 metrics record/atomic set을 사용한다. L1은 permit 할당·대기·반납을 추가한다. L2는 envelope allocation·parse, L3는 service frame allocation/copy·unpack, L4는 typed JSON allocation·codec을 추가한다. L0~L4 응답은 요청 part를 재사용하지 않고 새 reply를 인코딩해 두 part를 제출한다. L5~L7은 production owner queue/worker와 Framework dispatch·completion 진행을 추가하며 typed JSON reply도 새로 인코딩한다. 상세 production 경로는 `../lad-java-impl4/call-shape-L4.md`와 `../lad-java-impl5/call-shape-L5-7.md`에 기록돼 있다.
