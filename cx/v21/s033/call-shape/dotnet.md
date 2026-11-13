# .NET call-shape: L0

**NOT a restored feature — L0 source selects `frameworkTarget: true`, changing the header bytes (Framework request envelope for request and command) from the raw target's legacy request/command headers. The two-part binding submit remains the same.** (`Client/Program.cs:385-391,470-490,513-527`.)

비교 범위: raw binding target `ZLinkRawServer`와 L0 target `ZLinkServer`의 `ZLINK_DOTNET_LADDER_L0` 분기. ladder client는 `ZLINK_DOTNET_LADDER_CLIENT=raw`일 때 framework target을 가리키는 `RawBenchTransport`를 사용한다 (`Client/Program.cs:162-170`). 측정용 payload 생성과 per-stream metric 기록은 공통 driver를 따른다.

| path step | binding call (file:line) | L-step call (file:line) | same? | if different: restored feature |
|---|---|---|---|---|
| Source send — request/command: header/body 두 메시지와 async submit | `framework/bench/grpc/dotnet/Client/Program.cs:470-490,513-527,678-701` | framework target 설정 `Client/Program.cs:162-170`, header choice `Client/Program.cs:385-391`; submit `Client/Program.cs:470-490,513-527,678-701` | no | **NOT a restored feature** |
| Target wait/receive — `RecvFlags.None`, 100 ms socket receive timeout, 한 번에 한 메시지 처리 | `framework/bench/grpc/dotnet/ZLinkRawServer/Program.cs:37-46,64-73,119-138` | `framework/bench/grpc/dotnet/ZLinkServer/Program.cs:43-58,84-127` | yes | — |
| Target per-message metrics — request는 `RecordReceived`, command는 `Record` | `framework/bench/grpc/dotnet/ZLinkRawServer/Program.cs:74-77,135-138` | `framework/bench/grpc/dotnet/ZLinkServer/Program.cs:100-110`; handler `Program.cs:182-195` | yes | — |
| Target payload decode — 마지막 부분의 protobuf `BenchPayload` decode | `framework/bench/grpc/dotnet/ZLinkRawServer/Program.cs:74-76,135-137,153-161` | `framework/bench/grpc/dotnet/ZLinkServer/Program.cs:100-110,130-131,182-195` | yes | — |
| Target reply build/submit — 새 response header와 protobuf body를 두 부분으로 제출 | `framework/bench/grpc/dotnet/ZLinkRawServer/Program.cs:103-116` | `framework/bench/grpc/dotnet/ZLinkServer/Program.cs:100-107,182-190` | yes | — |
| Source completion/reply receive — binding request의 reply task decode; command async send 완료 | `framework/bench/grpc/dotnet/Client/Program.cs:470-490,572-595,687-701` | 같은 RawBenchTransport 호출 경로 `Client/Program.cs:470-490,572-595,687-701` | yes | — |
| Source per-message metrics — Begin/Complete의 카운터·lock·latency 샘플 갱신 | `framework/bench/grpc/dotnet/Client/Program.cs:712-795` | 같은 공통 `SourceMetrics` `Client/Program.cs:712-795` | yes | — |

수신 fix 반영 커밋 `a542ee2384`에서 raw와 L0 모두 timeout을 둔 `RecvFlags.None` 경로다. raw의 `NoData` 예외 처리는 timeout idle 반환 처리이며, L0도 `NoData`를 계속 처리한다 (`ZLinkRawServer/Program.cs:87-94`, `ZLinkServer/Program.cs:113-121`).
