# .NET L3/L4 profile 호출 대조

기준은 ladder tip `29d9fca82b`이며 scratch는 `/root/worktree/zlink-v21-s033/d4json-scratch`다. 코드는 지정 commit의 `git show`로 확인했다. 처리량 판정은 하지 않는다. 사용자 지시가 공통 측정 규칙의 CPU·lock·실행 시간 조건보다 우선한다: 전 프로세스 CPU 6-7,14-15, measure.lock 미사용, 각 profile 최대 20초.

| 경로 | binding | L3 | L4 | 차이의 소유 기능 |
|---|---|---|---|---|
| source send | Client/Program.cs:777-788 raw protobuf를 Message.AsSpan에 기록 | :752-775 동일 protobuf + :631-650 service framing | :754-769 BenchTypedPayload(byte[] Body), 기본 typed JSON envelope encode | L2 envelope, L3 multipart, L4 typed JSON |
| target receive | ZLinkRawServer/Program.cs:64-76 blocking Recv, protobuf decode | ZLinkServer/Program.cs:254-280 permit + 같은 Recv, :375-424 frame/header decode | 같은 경로, :410-437 기본 typed JSON decode | L1 permit, L2/L3 decode, L4 typed JSON |
| target reply | ZLinkRawServer/Program.cs:103-109 새 protobuf body/header, 두 part 제출 | ZLinkServer/Program.cs:427-457 새 protobuf reply + envelope/service framing | :459-477 새 typed JSON reply + 동일 framing | L2/L3 framing, L4 typed JSON |
| source completion | Client/Program.cs:792-890 binding completion 및 protobuf decode | :811-854 service reply/frame unpack, protobuf decode | :834-848 typed JSON decode 후 기존 ByteString 반환 | L3 framing, L4 typed JSON |

| 메시지당 자원 | L3 | L4 추가분 |
|---|---|---|
| wait | binding completion + target blocking Recv + permit | 없음 |
| metrics/atomic | 기존 bench counters + permit | 없음 |
| payload encode | protobuf bytes → native Message 직접 기록 | source Body.ToByteArray + DTO, production SerializeToUtf8Bytes + Message.From |
| payload decode | protobuf decode | production JSON 검증 + typed decode, source reply는 ByteString.CopyFrom |

bench DTO·reply 생성·completion은 바꾸지 않는다. L4의 Body.ToByteArray 및 ByteString.CopyFrom은 production serializer 밖의 비용이므로 production serializer 개선 효과와 분리한다.
