# .NET L1/L2 호출 대조

Scratch bench는 `69891f4446`을 git archive로 추출했다. source/target은 같은 L2 Release binary에서 환경 변수 L1/L2만 바꾼다. production reference의 MSBuild import만 이 worktree 절대경로로 연결했다. friend assembly는 외부 MSBuild targets로 주입하며 production source는 아직 수정하지 않았다.

| 단계 | Binding bench (69891f4446) | L1/L2 bench (69891f4446) | 차이 / 복구 기능 |
|---|---|---|---|
| source send | Client/Program.cs:436-452,509-526,700-708 raw header+새 protobuf body, Request().Message(header).Message(body).Async | 같은 SubmitRequest. L1 raw header; L2 CreateEnvelope→EncodeHeader(:443-449) | L2 header encode만 |
| target receive | ZLinkRawServer/Program.cs:64-76 blocking Recv / protobuf parse+기존 metrics | ZLinkServer/Program.cs:226-243,362-369 blocking Recv / 같은 protobuf parse+metrics | L1 permit 획득·반환; L2 DecodeHeader 1회(:234) |
| target reply | ZLinkRawServer/Program.cs:103-109 새 protobuf body/header·2parts submit | ZLinkServer/Program.cs:239-250,283-304,362-369 새 protobuf body·2parts submit | L1 raw response; L2 CreateReplyHeader→EncodeRawBodyParts. request part 재사용 없음 |
| source completion/reply | Client/Program.cs:691-692,592-626 native completion + RawWire.Decode(span) | 같은 코드; L2만 :608-611 header parse | L2 reply header parse 1회. production operation registry는 이 단계에 없음 |

L1/L2 모두 blocking receive 1회, application permit acquire/release 1쌍; 같은 metrics, binding completion waits. L2 추가: source Header+ID+Message 3개와 reply decode 4개; target request decode5개와 reply Header+Message+TwoMessageParts3개. Header byte 추가 복사: encode prefix1회, decode native span이라0회(값 string 생성 별도). Body protobuf decode/encode와 body bytes 복사는 L1과 같다. 실측 allocation probe는 raw/alloc-before.json.

프로파일 전용 CPU는 양쪽 모두6–7,14–15이며 measure.lock을 취득하지 않는다. 처리량 판정을 하지 않는다.
