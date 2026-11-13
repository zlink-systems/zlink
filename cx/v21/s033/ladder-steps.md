# 사다리 단계 정의 (PM 11-13 18:05 확정, 네 언어 공통)

| 단계 | 더하는 것 (production 경로 그대로) | 송신 | 수신 | 응답 | completion |
|---|---|---|---|---|---|
| L0 | binding과 같은 raw 경로 + 고정 handler 직접 호출 | raw | raw, binding과 같은 수신 방식 | raw | raw |
| L1 | application job queue permit (수신 전 획득, handler 직전 반환) | — | permit | — | — |
| L2 | envelope header: 요청·응답 모두 | 요청 envelope encode | header parse 1회 | 응답 envelope encode | 응답 header parse 1회 |
| L3 | service wire multipart framing | frame pack | frame unpack | frame pack | frame unpack |
| L4 | typed payload codec | serialize | deserialize | serialize | deserialize |
| L5~L7 | owner queue·worker, handler 활성화·DI scope, operation registry·completion·deadline (나눌 수 없는 언어는 한 단계, 예산 21%) | operation 등록 | owner queue → worker → 활성화 | completion dispatcher | operation 완료 |
| Node L0R/L4R | 해당 단계 모듈을 require만 함(모듈 로드 비용 분리) | — | — | — | — |

| 언어 | L2 응답 envelope | L5~L7 |
|---|---|---|
| C++ | 있음 fbdcb16e26 | 묶음 62e766f88d |
| .NET | 있음 69891f4446 | 묶음 2b6da5af64 (L4 JSON 수정 뒤 다시 쌓음) |
| Java | 있음 e8721a0776 | 묶음 f3e3933567 |
| Node | 있음 2bef18aa94 | 묶음 7d1afa98a9 |

## L4 serializer 대조 (#4354: 기본 typed JSON serializer, 메시지별 codec 등록 없음) — 11-13 19:05 UTC

L0~L3의 body는 binding과 같은 protobuf bytes입니다(L2·L3 응답 envelope의 protobuf content type도 이것). typed codec은 L4부터입니다.

| 언어 | L4 serializer | codec 등록 | bench file:line | L5~L7 |
|---|---|---|---|---|
| C++ | 기본 typed JSON (`I/contracts/codecs/serializer.hpp:345-375`) | 없음 | `B/ladder/ladder_payload.hpp:21-26`, `ladder_wire.hpp:132-136,278-296` (276b84f248) | level 7에서 protobuf 등록 끔 `bench_framework_cpp_server.cpp:82` (62e766f88d) |
| Java | 기본 JSON `ZLinkJsonMessageSerializer` | 없음 | `RawStack.java:481`, `ZLinkFrameworkBenchServer.java:348` (9d9901eb2c) | typed DTO `BenchTypedPayload`(record) → JSON. 서버·client 공통 설정에 `ZLinkProtobufCodec` 등록이 남아 있으나 protobuf 메시지 전용이라 이 DTO에는 쓰이지 않음 (f3e3933567) |
| .NET | **protobuf** `ZLinkProtobufCodec` | **있음** `ZLinkServer/Program.cs:178` | (1bcaebde9e) | 수정 중: lad-dotnet-l4j가 기본 JSON으로 바꾸고 다시 쌓음 |
| Node | 기본 typed JSON (codec 인자 없는 `encodeChannelEnvelopeParts`/`encodeChannelReplyParts`, content type `application/json`) | 없음 | `shared/raw-wire.js:119,135,151` (ae8a5178b5) | L5_7 server·client에서 protobuf 등록 끔 `zlink-framework-server/main.js:345-354`, `client/framework-transport.js:28-40` (7d1afa98a9) |
