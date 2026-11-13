# .NET ladder L2 reply 복원

- L2 reply production 경로: `ZLinkChannelRequestDispatchPipeline.cs:132-143` → `ZLinkChannelReplyWriter.cs:5-16,34-57` → `ZLinkEnvelopeCodec.cs:151-167` (raw body encoder `175-181`). Completion parse: `ZLinkRouteClient.cs:232-254` → `ZLinkClientCallCodec.cs:77-97` → `ZLinkEnvelopeCodec.cs:374-375`.
- L2 bench 경로: target reply `framework/bench/grpc/dotnet/ZLinkServer/Program.cs:150-164,262-281`; source completion parse `framework/bench/grpc/dotnet/Client/Program.cs:592-611`. Reply response는 request correlation을 보존하며 protobuf body와 함께 2 parts로 제출한다.
- 새 L2 commit: `638211bbd3d16508bc6026811b23ad9f27341d7f`.

| 재적용 단계 | 이전 commit | 새 commit |
|---|---|---|
| scheduler capture | `bfc15a8dd6` | `98bb7ed5981ec3548e423a10e480041f71e527f8` |
| L3 service wire | `b8eb5589e7` | `c4bf6449ffde0e19c8987afcb1e500b85e2c2ace` |
| L4 typed codec | `a7fd48d1b1` | `f94b924ebbab730b215d1e72b9a82be466999bde` |
| L5–L7 owner dispatch | `de0507ae48` | `a03bf0bfe07a246a3c46ec0a596c47fbe4938bd8` |

- 검증: L2 Release solution build 통과. tip `L5_7` Release solution build 통과. WireTests 통과. tip BoundaryTests의 request-serial, request-window, send-saturation 통과.
- 미완료 검사: L2 BoundaryTests는 request-serial/window 통과 후 request-backpressure에서 2분 넘게 진행되지 않아 종료했다. 측정·benchmark 실행은 없다.
- `lad-dotnet-m1/call-shape.md`에 L2 target reply/source completion 행과 production/bench file:line을 추가했다. `tmp-l2-reply` 브랜치는 삭제했다.
