# .NET ladder L4 serializer 수정 보고

## 결과

L4는 일반 DTO `BenchTypedPayload`로 전환했다. source encode와 target reply encode는 `ZLinkEnvelopeCodec.EncodeEnvelopeParts`/`EncodeParts`에 serializer를 `null`로 전달하고, target decode와 source completion decode도 codec registry 없이 `DecodeBody`를 호출한다. Framework 기본 선택은 [ZLinkEnvelopeCodec.cs:342-359, 839-852, 902-909](/root/worktree/zlink-v21-s033/lad-dotnet/framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkEnvelopeCodec.cs)에서 null serializer의 JSON encode와 빈 기본 registry의 수신 serializer 선택으로 처리된다. 기본 registry는 [ZLinkCodecRegistryBuilder.cs:106-110, 133-137](/root/worktree/zlink-v21-s033/lad-dotnet/framework/languages/dotnet/src/Zlink.Framework/Runtime/Codecs/ZLinkCodecRegistryBuilder.cs)에서 JSON serializer를 선택하고, [ZLinkFrameworkJsonPayloadCodec.cs:5-33](/root/worktree/zlink-v21-s033/lad-dotnet/framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkFrameworkJsonPayloadCodec.cs)가 Framework JSON 구현을 제공한다.

## 벤치 경로

- L4 source encode: [Client/Program.cs:752-768](/root/worktree/zlink-v21-s033/lad-dotnet/framework/bench/grpc/dotnet/Client/Program.cs)
- L4 target decode: [ZLinkServer/Program.cs:406-436](/root/worktree/zlink-v21-s033/lad-dotnet/framework/bench/grpc/dotnet/ZLinkServer/Program.cs)
- L4 target reply encode: [ZLinkServer/Program.cs:459-477](/root/worktree/zlink-v21-s033/lad-dotnet/framework/bench/grpc/dotnet/ZLinkServer/Program.cs)
- L4 source completion decode: [Client/Program.cs:792-850](/root/worktree/zlink-v21-s033/lad-dotnet/framework/bench/grpc/dotnet/Client/Program.cs)
- L5~L7 target public typed route: [ZLinkServer/Program.cs:25-40](/root/worktree/zlink-v21-s033/lad-dotnet/framework/bench/grpc/dotnet/ZLinkServer/Program.cs)
- L5~L7 source public typed route: [Client/Program.cs:324-370, 383-410](/root/worktree/zlink-v21-s033/lad-dotnet/framework/bench/grpc/dotnet/Client/Program.cs)

## 커밋

- L4 `1bcaebde9e` → `4cbe0b5316`
- L5~L7 `2b6da5af64` → `29d9fca82b`
- 로컬 branch `exp/4350-dotnet-ladder`; push와 PR 없음.

## 검증

- L4 Release solution build: 성공, 0 errors (`raw/l4-release-build.log`)
- tip Release solution build: 성공, 0 errors (`raw/tip-release-build.log`)
- tip WireTests: 통과 (`raw/tip-wiretests.log`)
- tip BoundaryTests: request-serial 및 request-window 통과. `grpc-dotnet-request-backpressure`에서 실패 후 종료: submitted 30,850, completed 6,000, errors 3, abandoned 24,847 (`raw/tip-boundary-tests.log`). 나머지 BoundaryTests 항목은 실행되지 않음. 실패는 Framework ladder의 DTO 경로가 아닌 gRPC transport에서 발생했으며 원인은 미확정.
- L4와 L5~L7 dry start/stop: 각각 시작·ready·SIGTERM 종료 성공, 요청 부하 없음 (`raw/l4-dry-start-stop.log`, `raw/tip-dry-start-stop.log`).
- 벤치마크/측정 및 load 실행 없음. Raw 로그는 보존함.

## 원칙 점검

- serializer 결정 소유자: Framework `ZLinkEnvelopeCodec` 기본 JSON 경로. 요구 조항: #4354의 기본 typed JSON serializer 요구.
- 교차언어 대조: C++ `276b84f248`, Java `9d9901eb2c`의 일반 DTO 기본 JSON 경로와 일치.
- 분류: B, 벤치 L4의 기존 serializer 경로 불일치 수정.
- 규칙 수: L4 명시 protobuf serializer 선택 1곳 → 기본 JSON serializer 선택 1곳. 변경 모듈 호출 경로의 성능·POSDDD·불필요 코드 점검에서 추가 발견 없음.
