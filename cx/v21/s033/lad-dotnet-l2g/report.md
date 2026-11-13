# .NET ladder L2 reply 수정 및 BoundaryTests 조사

## request-backpressure 지연 원인

`BoundaryTests`는 로컬 Kestrel gRPC 서버와 `GrpcBenchTransport`를 사용하며 ZLink ladder reply 경로를 호출하지 않는다. 이 경로는 `Client/Program.cs`의 `GrpcBenchTransport.RequestAsync`를 호출한다. backpressure 시험은 [BenchMeasurement.cs](/root/worktree/zlink-v21-s033/lad-dotnet/framework/bench/grpc/dotnet/Client/BenchMeasurement.cs)의 `RunRequestBackpressureStreams`에서 pending task를 모으고, `RunDedicatedStreams`가 각 worker thread의 종료를 기다린다. 저장한 stack은 이 두 메서드의 대기 상태를 보인다.

TIP 전체 BoundaryTests는 네 케이스 모두 통과했다. backpressure는 요청 92,038개를 제출한 뒤 2분 넘게 pending 요청을 정리하고 정상 종료했다 (`raw/tip-fixed-all/test.log`). 따라서 이전의 “stall”은 L2 reply 결함이 아니라 이 테스트의 대량 gRPC drain 시간이다. L1과 원본 L2 제한 실행에서 얻은 stack/dump도 저장했다. 이 동작은 ladder 코드 수정 대상이 아니며 Framework 변경은 하지 않았다.

- L1은 60초 제한에서 종료했다. 종료 직전 RSS는 4.6 GiB였다 (`raw/l1-ab21/status.txt`). stack과 dump: `raw/l1-ab21/at-40s/stack.txt`, `stall.dmp`
- 원본 L2 60초 제한 dump: `raw/l2-original/stack.txt`, `stall.dmp`
- TIP 단독 재현 stack/dump: `raw/tip-original-110s/at-60s/stack.txt`, `stall.dmp`; 이후 단독 케이스는 정상 완료
- 수정 후 L2 전체 BoundaryTests는 240초 제한 안에 완료했다 (`raw/l2-fixed-all-final/test.log`, `status.txt`).
- TIP 전체 재현 명령과 결과: `raw/tip-fixed-all/repro-command.txt`, `test.log`, `status.txt`

## L2 reply 수정

Production은 [ZLinkHandlerRegistry.cs](/root/worktree/zlink-v21-s033/lad-dotnet/framework/languages/dotnet/src/Zlink.Framework/Runtime/Handlers/ZLinkHandlerRegistry.cs:68)에서 endpoint마다 한 번 `PrepareResponse`를 만들고, [ZLinkEnvelopeCodec.cs](/root/worktree/zlink-v21-s033/lad-dotnet/framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkEnvelopeCodec.cs:245)에서 reply type의 serializer가 정한 content type을 넣는다. 실제 dispatch는 [ZLinkChannelRequestDispatchPipeline.cs](/root/worktree/zlink-v21-s033/lad-dotnet/framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkChannelRequestDispatchPipeline.cs:132)에서 준비된 header를 `CreateReplyHeader`에 전달한다.

Bench는 [Program.cs](/root/worktree/zlink-v21-s033/lad-dotnet/framework/bench/grpc/dotnet/ZLinkServer/Program.cs:89)에서 같은 protobuf codec으로 prepared header를 서버 시작 때 만든다. `ReplyL2` [구현](/root/worktree/zlink-v21-s033/lad-dotnet/framework/bench/grpc/dotnet/ZLinkServer/Program.cs:356)은 request header의 channel name을 사용하고, prepared header를 reply마다 한 번만 복사한다. 고정 채널명과 추가 `with { ContentType = ... }` 복사는 제거했다.

## commit 재적용

| 단계 | 이전 | 새 |
|---|---|---|
| L2 reply | `638211bbd3` | `69891f4446` |
| scheduler capture | `bfc15a8dd6` | `bdb801fd2e` |
| L3 service wire | `b8eb5589e7` | `e8ee8302b0` |
| L4 typed codec | `a7fd48d1b1` | `1bcaebde9e` |
| L5–L7 owner dispatch | `de0507ae48` | `2b6da5af64` |

## 검증

- L2 Release solution build: 통과, 오류 0개 (`raw/l2-fixed-build-final.log`; 경고 30개)
- TIP Release solution build: 통과, 오류 0개 (`raw/tip-build.log`, 경고 10개)
- WireTests: L2와 TIP 모두 통과 (`raw/l2-fixed-wiretests-final.log`, `raw/tip-wiretests.log`)
- BoundaryTests: L2와 TIP 모두 전체 케이스 통과 (`raw/l2-fixed-all-final/test.log`, `raw/tip-fixed-all/test.log`)
- benchmark와 load는 실행하지 않았다. 원격 저장소에 push하지 않았고 PR도 만들지 않았다.
