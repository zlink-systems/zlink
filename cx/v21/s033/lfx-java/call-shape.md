# Java ladder 호출 비교 — fix branch

| 경로 | Binding bench | L2 | L3 | L4 | 동일 여부와 복원 기능 |
|---|---|---|---|---|---|
| Source send | `RawStack.java:162-191,309-339` | `FrameworkStack.java:159-162` → `RawStack.sendWithEnvelope` | `FrameworkStack.java:152-155` → `sendWithServiceWireEnvelope` | `FrameworkStack.java:145-148` → `sendWithTypedJsonServiceWireEnvelope` | submit, driver, 계측 동일. L2 envelope encode, L3 service-wire frame pack, L4 typed JSON 직렬화 추가. |
| Target receive | `ZLinkRawBenchServer.java:94-109` | `ZLinkFrameworkBenchServer.java:151-177,211-237` | `ZLinkFrameworkBenchServer.java:151-177,351-419` | `ZLinkFrameworkBenchServer.java:151-177,272-316` | 모두 동일한 blocking `router.recv` owner loop. L2 header parse, L3 multipart frame unpack, L4 typed JSON deserialize 추가. |
| Target reply | `ZLinkRawBenchServer.java:115-127` | `ZLinkFrameworkBenchServer.java:249-269` | `ZLinkFrameworkBenchServer.java:400-419` | `ZLinkFrameworkBenchServer.java:291-337` | 매번 새 응답 body/envelope 생성, 같은 part 수·submit. L2 envelope encode, L3 frame pack, L4 typed JSON 직렬화 추가. 요청 parts 재사용 없음. |
| Source reply completion/receive | `RawStack.java:309-339` | `FrameworkStack.java:160-162` → `RawStack.requestWithEnvelope` | `FrameworkStack.java:153-155` → `requestWithServiceWireEnvelope` | `FrameworkStack.java:146-148` → `requestWithTypedJsonServiceWireEnvelope` | 같은 driver·completion 지점. L2 응답 header parse, L3 frame unpack, L4 typed JSON deserialize 추가. |

각 단계는 `ladder-steps.md`에 정한 기능만 추가한다. 수신 대기 횟수, driver metrics counter/atomic 수, 제출 completion 구조 및 application payload 크기는 공통이다. Binding 경로는 `framework/bench/grpc/java`의 production source이며 별도 helper는 없다.
