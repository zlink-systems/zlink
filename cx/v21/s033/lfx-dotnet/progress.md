# 진행 기록

- 2026-11-13 23:27 UTC: 현재 worktree branch `exp/4350-dotnet-fixes`, HEAD `4bd6102170ad5ff405cab7bccc74113aa71b4ea1`, merge-base `6350275facc547759c9e143cc2515f3f528c776e`, 시작 상태 clean. ladder bench commit 포함.
- 측정 전 call-shape: [call-shape.md](/root/jobs/fx/cx/v21/s033/lfx-dotnet/call-shape.md). binding/L1/L2/L3/L4의 send·receive·reply·completion 경로와 단계별 추가를 비교했다.
- runner는 lad-dotnet-s1 도구를 이 job 디렉터리로 복사하고 worktree/job 경로를 현재 경로로 변경했다.
- Release L5_7-compatible build 실행 중: [release-build.log](/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/release-build.log). 완료 후 rc, 소스보다 신규인 artifact, SHA256을 기록하고 모든 측정 전 progress에 다시 쓴다.
- Release build 재시도 성공(rc=0): [release-build-friend-target.log](/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/release-build-friend-target.log). 외부 `/tmp/lfx-dotnet-ladder-bench-friends.targets`가 bench friend assembly metadata만 Framework build에 제공했고, worktree는 계속 clean. artifacts는 Client `35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723`, ZLinkServer `f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332`, ZLinkRawServer `6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552`; 모두 관련 `.cs/.props/.csproj`보다 새로 생성됨.
- 측정 전 hold 검사(23:36 UTC): 다른 worktree의 `dotnet build`, CMake/Ninja 및 test/build 작업이 R/S 상태여서 common-rules의 BUILDS BETWEEN HOLDS에 따라 lock을 아직 취하지 않음. 시작 후 10분 이내 완료 여부를 확인한다.
- 23:46 UTC: 다른 worktree build 종료를 확인했다. 최초 검사 뒤 약 10분 대기했고 종료 후 첫 exclusive hold를 시작한다. build SHA/freshness 확인 결과는 위에 기록.
- 2026-11-13T23:47:27Z level=L1 cell=send-saturation-1024 stage=warm HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.
- warm level=L1 cell=send-saturation-1024 discarded msg/s=364421.2 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/send-saturation-1024/warm/attempt1/run1/zlink-dotnet-send-saturation-1024/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/send-saturation-1024/warm/attempt1/run1/zlink-dotnet-send-saturation-1024/scheduling.txt
- 2026-11-13T23:47:49Z level=L1 cell=send-saturation-1024 stage=counted HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.
- result level=L1 cell=send-saturation-1024 msg/s=358206.8 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/send-saturation-1024/counted/attempt1/run1/zlink-dotnet-send-saturation-1024/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/send-saturation-1024/counted/attempt1/run1/zlink-dotnet-send-saturation-1024/scheduling.txt
- 2026-11-13T23:48:10Z level=L1 cell=send-saturation-4096 stage=warm HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.
- warm level=L1 cell=send-saturation-4096 discarded msg/s=149576.2 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/send-saturation-4096/warm/attempt1/run1/zlink-dotnet-send-saturation-4096/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/send-saturation-4096/warm/attempt1/run1/zlink-dotnet-send-saturation-4096/scheduling.txt
- 2026-11-13T23:48:31Z level=L1 cell=send-saturation-4096 stage=counted HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.
- result level=L1 cell=send-saturation-4096 msg/s=148668.6 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/send-saturation-4096/counted/attempt1/run1/zlink-dotnet-send-saturation-4096/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/send-saturation-4096/counted/attempt1/run1/zlink-dotnet-send-saturation-4096/scheduling.txt
- 2026-11-13T23:48:52Z level=L1 cell=request-serial-1024 stage=warm HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.
- warm level=L1 cell=request-serial-1024 discarded msg/s=7574 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-serial-1024/warm/attempt1/run1/zlink-dotnet-request-serial-1024/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-serial-1024/warm/attempt1/run1/zlink-dotnet-request-serial-1024/scheduling.txt
- 2026-11-13T23:49:13Z level=L1 cell=request-serial-1024 stage=counted HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.
- result level=L1 cell=request-serial-1024 msg/s=7688 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-serial-1024/counted/attempt1/run1/zlink-dotnet-request-serial-1024/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-serial-1024/counted/attempt1/run1/zlink-dotnet-request-serial-1024/scheduling.txt
- 2026-11-13T23:49:34Z level=L1 cell=request-serial-4096 stage=warm HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.
- warm level=L1 cell=request-serial-4096 discarded msg/s=7162.6 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-serial-4096/warm/attempt1/run1/zlink-dotnet-request-serial-4096/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-serial-4096/warm/attempt1/run1/zlink-dotnet-request-serial-4096/scheduling.txt
- 2026-11-13T23:49:54Z level=L1 cell=request-serial-4096 stage=counted HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.
- result level=L1 cell=request-serial-4096 msg/s=7204.4 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-serial-4096/counted/attempt1/run1/zlink-dotnet-request-serial-4096/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-serial-4096/counted/attempt1/run1/zlink-dotnet-request-serial-4096/scheduling.txt
- 2026-11-13T23:50:15Z level=L1 cell=request-backpressure-1024 stage=warm HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.
- warm level=L1 cell=request-backpressure-1024 discarded msg/s=67928.8 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-backpressure-1024/warm/attempt1/run1/zlink-dotnet-request-backpressure-1024/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-backpressure-1024/warm/attempt1/run1/zlink-dotnet-request-backpressure-1024/scheduling.txt
- 2026-11-13T23:50:36Z level=L1 cell=request-backpressure-1024 stage=counted HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.
- result level=L1 cell=request-backpressure-1024 msg/s=66499.2 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-backpressure-1024/counted/attempt1/run1/zlink-dotnet-request-backpressure-1024/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-backpressure-1024/counted/attempt1/run1/zlink-dotnet-request-backpressure-1024/scheduling.txt
- 2026-11-13T23:50:57Z level=L1 cell=request-backpressure-4096 stage=warm HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.
- warm level=L1 cell=request-backpressure-4096 discarded msg/s=73192.8 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-backpressure-4096/warm/attempt1/run1/zlink-dotnet-request-backpressure-4096/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-backpressure-4096/warm/attempt1/run1/zlink-dotnet-request-backpressure-4096/scheduling.txt
- 2026-11-13T23:51:17Z level=L1 cell=request-backpressure-4096 stage=counted HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.
- result level=L1 cell=request-backpressure-4096 msg/s=71681.6 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-backpressure-4096/counted/attempt1/run1/zlink-dotnet-request-backpressure-4096/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-backpressure-4096/counted/attempt1/run1/zlink-dotnet-request-backpressure-4096/scheduling.txt
- 2026-11-13T23:51:38Z level=L2 cell=send-saturation-1024 stage=warm HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.
- warm level=L2 cell=send-saturation-1024 discarded msg/s=344936 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/send-saturation-1024/warm/attempt1/run1/zlink-dotnet-send-saturation-1024/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/send-saturation-1024/warm/attempt1/run1/zlink-dotnet-send-saturation-1024/scheduling.txt
- 2026-11-13T23:52:00Z level=L2 cell=send-saturation-1024 stage=counted HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.
- result level=L2 cell=send-saturation-1024 msg/s=343949.6 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/send-saturation-1024/counted/attempt1/run1/zlink-dotnet-send-saturation-1024/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/send-saturation-1024/counted/attempt1/run1/zlink-dotnet-send-saturation-1024/scheduling.txt
- 2026-11-13T23:52:21Z level=L2 cell=send-saturation-4096 stage=warm HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.
- warm level=L2 cell=send-saturation-4096 discarded msg/s=145689 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/send-saturation-4096/warm/attempt1/run1/zlink-dotnet-send-saturation-4096/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/send-saturation-4096/warm/attempt1/run1/zlink-dotnet-send-saturation-4096/scheduling.txt
- 2026-11-13T23:52:42Z level=L2 cell=send-saturation-4096 stage=counted HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.
- result level=L2 cell=send-saturation-4096 msg/s=145166 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/send-saturation-4096/counted/attempt1/run1/zlink-dotnet-send-saturation-4096/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/send-saturation-4096/counted/attempt1/run1/zlink-dotnet-send-saturation-4096/scheduling.txt
- 2026-11-13T23:53:04Z level=L2 cell=request-serial-1024 stage=warm HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.
- warm level=L2 cell=request-serial-1024 discarded msg/s=7144.2 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-serial-1024/warm/attempt1/run1/zlink-dotnet-request-serial-1024/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-serial-1024/warm/attempt1/run1/zlink-dotnet-request-serial-1024/scheduling.txt
- 2026-11-13T23:53:24Z level=L2 cell=request-serial-1024 stage=counted HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.
- result level=L2 cell=request-serial-1024 msg/s=7093.2 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-serial-1024/counted/attempt1/run1/zlink-dotnet-request-serial-1024/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-serial-1024/counted/attempt1/run1/zlink-dotnet-request-serial-1024/scheduling.txt
- 2026-11-13T23:53:45Z level=L2 cell=request-serial-4096 stage=warm HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.
- warm level=L2 cell=request-serial-4096 discarded msg/s=6712.8 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-serial-4096/warm/attempt1/run1/zlink-dotnet-request-serial-4096/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-serial-4096/warm/attempt1/run1/zlink-dotnet-request-serial-4096/scheduling.txt
- 2026-11-13T23:54:06Z level=L2 cell=request-serial-4096 stage=counted HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.
- result level=L2 cell=request-serial-4096 msg/s=6612.6 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-serial-4096/counted/attempt1/run1/zlink-dotnet-request-serial-4096/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-serial-4096/counted/attempt1/run1/zlink-dotnet-request-serial-4096/scheduling.txt
- 2026-11-13T23:54:26Z level=L2 cell=request-backpressure-1024 stage=warm HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.
- warm level=L2 cell=request-backpressure-1024 discarded msg/s=66032.4 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-backpressure-1024/warm/attempt1/run1/zlink-dotnet-request-backpressure-1024/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-backpressure-1024/warm/attempt1/run1/zlink-dotnet-request-backpressure-1024/scheduling.txt
- 2026-11-13T23:54:47Z level=L2 cell=request-backpressure-1024 stage=counted HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.
- result level=L2 cell=request-backpressure-1024 msg/s=65142.6 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-backpressure-1024/counted/attempt1/run1/zlink-dotnet-request-backpressure-1024/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-backpressure-1024/counted/attempt1/run1/zlink-dotnet-request-backpressure-1024/scheduling.txt
- 2026-11-13T23:55:08Z level=L2 cell=request-backpressure-4096 stage=warm HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.
- warm level=L2 cell=request-backpressure-4096 discarded msg/s=68866.8 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-backpressure-4096/warm/attempt1/run1/zlink-dotnet-request-backpressure-4096/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-backpressure-4096/warm/attempt1/run1/zlink-dotnet-request-backpressure-4096/scheduling.txt
- 2026-11-13T23:55:29Z level=L2 cell=request-backpressure-4096 stage=counted HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.
- result level=L2 cell=request-backpressure-4096 msg/s=70038.8 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-backpressure-4096/counted/attempt1/run1/zlink-dotnet-request-backpressure-4096/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-backpressure-4096/counted/attempt1/run1/zlink-dotnet-request-backpressure-4096/scheduling.txt
- 2026-11-13T23:58:24Z level=L1 cell=request-serial-1024 stage=warm HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding; metrics: same target record; copies: protobuf body.
- Boundary-repeat hold 1 (23:57:30–23:58:45 UTC) rc=1. 첫 L1 request-serial 1K warm run은 source thread 2231674/2231734/2231737가 `SCHED_IDLE`이라 invalid; 해당 warm raw는 보존하고 verdict에서 제외. No counted repeat was accepted. 다른 build 상태는 [builds-before-boundary-repeat.txt](/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/builds-before-boundary-repeat.txt)에 기록.
- 2026-11-14T00:14:33Z level=L1 cell=request-serial-1024 stage=warm HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding; metrics: same target record; copies: protobuf body.
- repeat level=L1 cell=request-serial-1024 stage=warm attempt=3 rc=1 output=Traceback (most recent call last):
  File "<stdin>", line 6, in <module>
AssertionError: run_start_utc=2026-11-14T00:14:36Z
cpu0.state2.disable_at_run_start=1
cpu0.state3.disable_at_run_start=1
cpu8.state2.disable_at_run_start=1
cpu8.state3.disable_at_run_start=1

source_start_utc=2026-11-14T00:14:36Z
source_start_pid=2293659
source_start_process_pid=2293659
source_start_tid=2293659
source_start_affinity tid=2293659 cpus=0-5
pid 2293659's current scheduling policy: SCHED_OTHER
pid 2293659's current scheduling priority: 0
source_start_tid=2293663
source_start_affinity tid=2293663 cpus=0-5
pid 2293663's current scheduling policy: SCHED_OTHER
pid 2293663's current scheduling priority: 0
source_start_tid=2293664
source_start_affinity tid=2293664 cpus=0-5
pid 2293664's current scheduling policy: SCHED_OTHER
pid 2293664's current scheduling priority: 0
source_start_tid=2293665
source_start_affinity tid=2293665 cpus=0-5
pid 2293665's current scheduling policy: SCHED_OTHER
pid 2293665's current scheduling priority: 0
source_start_tid=2293666
source_start_affinity tid=2293666 cpus=0-5
pid 2293666's current scheduling policy: SCHED_OTHER
pid 2293666's current scheduling priority: 0
source_start_tid=2293667
source_start_affinity tid=2293667 cpus=0-5
pid 2293667's current scheduling policy: SCHED_OTHER
pid 2293667's current scheduling priority: 0
source_start_tid=2293668
source_start_affinity tid=2293668 cpus=0-5
pid 2293668's current scheduling policy: SCHED_OTHER
pid 2293668's current scheduling priority: 0
source_start_tid=2293669
source_start_affinity tid=2293669 cpus=0-5
pid 2293669's current scheduling policy: SCHED_OTHER
pid 2293669's current scheduling priority: 0
source_start_tid=2293670
source_start_affinity tid=2293670 cpus=0-5
pid 2293670's current scheduling policy: SCHED_OTHER
pid 2293670's current scheduling priority: 0
source_start_tid=2293671
source_start_affinity tid=2293671 cpus=0-5
pid 2293671's current scheduling policy: SCHED_OTHER
pid 2293671's current scheduling priority: 0
source_start_tid=2293715
source_start_affinity tid=2293715 cpus=0-5
pid 2293715's current scheduling policy: SCHED_OTHER
pid 2293715's current scheduling priority: 0
source_start_tid=2293716
source_start_affinity tid=2293716 cpus=0-5
pid 2293716's current scheduling policy: SCHED_OTHER
pid 2293716's current scheduling priority: 0
source_start_tid=2293717
source_start_affinity tid=2293717 cpus=0-5
pid 2293717's current scheduling policy: SCHED_OTHER
pid 2293717's current scheduling priority: 0
source_start_tid=2293719
source_start_affinity tid=2293719 cpus=0-5
pid 2293719's current scheduling policy: SCHED_OTHER
pid 2293719's current scheduling priority: 0
source_start_tid=2293756
source_start_affinity tid=2293756 cpus=0-5
pid 2293756's current scheduling policy: SCHED_IDLE
pid 2293756's current scheduling priority: 0
scheduling_invalid=source_start:2293756
source_start_tid=2293759
source_start_affinity tid=2293759 cpus=0-5
pid 2293759's current scheduling policy: SCHED_IDLE
pid 2293759's current scheduling priority: 0
scheduling_invalid=source_start:2293759
source_start_tid=2293773
source_start_affinity tid=2293773 cpus=0-5
pid 2293773's current scheduling policy: SCHED_OTHER
pid 2293773's current scheduling priority: 0
source_start_process_pid=2293771
source_start_tid=2293771
source_start_affinity tid=2293771 cpus=0-5
pid 2293771's current scheduling policy: SCHED_OTHER
pid 2293771's current scheduling priority: 0
source_start_tid=2293777
source_start_affinity tid=2293777 cpus=0-5
pid 2293777's current scheduling policy: SCHED_OTHER
pid 2293777's current scheduling priority: 0
source_start_tid=2293778
source_start_affinity tid=2293778 cpus=0-5
pid 2293778's current scheduling policy: SCHED_OTHER
pid 2293778's current scheduling priority: 0
source_start_tid=2293779
source_start_affinity tid=2293779 cpus=0-5
pid 2293779's current scheduling policy: SCHED_OTHER
pid 2293779's current scheduling priority: 0
source_start_tid=2293781
source_start_affinity tid=2293781 cpus=0-5
pid 2293781's current scheduling policy: SCHED_OTHER
pid 2293781's current scheduling priority: 0
source_start_tid=2293783
source_start_affinity tid=2293783 cpus=0-5
pid 2293783's current scheduling policy: SCHED_OTHER
pid 2293783's current scheduling priority: 0
source_start_tid=2293784
source_start_affinity tid=2293784 cpus=0-5
pid 2293784's current scheduling policy: SCHED_OTHER
pid 2293784's current scheduling priority: 0
source_start_tid=2293785
source_start_affinity tid=2293785 cpus=0-5
pid 2293785's current scheduling policy: SCHED_OTHER
pid 2293785's current scheduling priority: 0
source_start_tid=2293793
source_start_affinity tid=2293793 cpus=0-5
pid 2293793's current scheduling policy: SCHED_OTHER
pid 2293793's current scheduling priority: 0
source_start_tid=2293815
source_start_affinity tid=2293815 cpus=0-5
pid 2293815's current scheduling policy: SCHED_OTHER
pid 2293815's current scheduling priority: 0
source_start_tid=2293816
source_start_affinity tid=2293816 cpus=0-5
pid 2293816's current scheduling policy: SCHED_OTHER
pid 2293816's current scheduling priority: 0
source_start_tid=2293817
source_start_affinity tid=2293817 cpus=0-5
pid 2293817's current scheduling policy: SCHED_OTHER
pid 2293817's current scheduling priority: 0
source_start_tid=2293818
source_start_affinity tid=2293818 cpus=0-5
pid 2293818's current scheduling policy: SCHED_OTHER
pid 2293818's current scheduling priority: 0
source_start_tid=2293819
source_start_affinity tid=2293819 cpus=0-5
pid 2293819's current scheduling policy: SCHED_OTHER
pid 2293819's current scheduling priority: 0
source_start_tid=2293820
source_start_affinity tid=2293820 cpus=0-5
pid 2293820's current scheduling policy: SCHED_OTHER
pid 2293820's current scheduling priority: 0
source_start_tid=2293825
source_start_affinity tid=2293825 cpus=0-5
pid 2293825's current scheduling policy: SCHED_OTHER
pid 2293825's current scheduling priority: 0
source_start_tid=2293838
source_start_affinity tid=2293838 cpus=0-5
pid 2293838's current scheduling policy: SCHED_OTHER
pid 2293838's current scheduling priority: 0
source_start_tid=2293851
source_start_affinity tid=2293851 cpus=0-5
pid 2293851's current scheduling policy: SCHED_OTHER
pid 2293851's current scheduling priority: 0
source_start_tid=2293852
source_start_affinity tid=2293852 cpus=0-5
pid 2293852's current scheduling policy: SCHED_OTHER
pid 2293852's current scheduling priority: 0
source_start_tid=2293853
source_start_affinity tid=2293853 cpus=0-5
pid 2293853's current scheduling policy: SCHED_OTHER
pid 2293853's current scheduling priority: 0
source_start_tid=2293905
source_start_affinity tid=2293905 cpus=0-5
pid 2293905's current scheduling policy: SCHED_OTHER
pid 2293905's current scheduling priority: 0
source_start_tid=2293913
source_start_affinity tid=2293913 cpus=0-5
pid 2293913's current scheduling policy: SCHED_OTHER
pid 2293913's current scheduling priority: 0
source_start_tid=2293915
source_start_affinity tid=2293915 cpus=0-5
pid 2293915's current scheduling policy: SCHED_OTHER
pid 2293915's current scheduling priority: 0
source_start_tid=2293919
source_start_affinity tid=2293919 cpus=0-5
pid 2293919's current scheduling policy: SCHED_OTHER
pid 2293919's current scheduling priority: 0
source_start_tid=2293922
source_start_affinity tid=2293922 cpus=0-5
pid 2293922's current scheduling policy: SCHED_OTHER
pid 2293922's current scheduling priority: 0
source_start_tid=2293924
source_start_affinity tid=2293924 cpus=0-5
pid 2293924's current scheduling policy: SCHED_OTHER
pid 2293924's current scheduling priority: 0
source_start_tid=2293974
source_start_affinity tid=2293974 cpus=0-5
pid 2293974's current scheduling policy: SCHED_OTHER
pid 2293974's current scheduling priority: 0
source_start_tid=2293975
source_start_affinity tid=2293975 cpus=0-5
pid 2293975's current scheduling policy: SCHED_OTHER
pid 2293975's current scheduling priority: 0

target_start_utc=2026-11-14T00:14:36Z
target_start_pid=2293406
target_start_process_pid=2293406
target_start_tid=2293406
target_start_affinity tid=2293406 cpus=8-13
pid 2293406's current scheduling policy: SCHED_OTHER
pid 2293406's current scheduling priority: 0
target_start_tid=2293412
target_start_affinity tid=2293412 cpus=8-13
pid 2293412's current scheduling policy: SCHED_OTHER
pid 2293412's current scheduling priority: 0
target_start_tid=2293413
target_start_affinity tid=2293413 cpus=8-13
pid 2293413's current scheduling policy: SCHED_OTHER
pid 2293413's current scheduling priority: 0
target_start_tid=2293415
target_start_affinity tid=2293415 cpus=8-13
pid 2293415's current scheduling policy: SCHED_OTHER
pid 2293415's current scheduling priority: 0
target_start_tid=2293416
target_start_affinity tid=2293416 cpus=8-13
pid 2293416's current scheduling policy: SCHED_OTHER
pid 2293416's current scheduling priority: 0
target_start_tid=2293417
target_start_affinity tid=2293417 cpus=8-13
pid 2293417's current scheduling policy: SCHED_OTHER
pid 2293417's current scheduling priority: 0
target_start_tid=2293418
target_start_affinity tid=2293418 cpus=8-13
pid 2293418's current scheduling policy: SCHED_OTHER
pid 2293418's current scheduling priority: 0
target_start_tid=2293419
target_start_affinity tid=2293419 cpus=8-13
pid 2293419's current scheduling policy: SCHED_OTHER
pid 2293419's current scheduling priority: 0
target_start_tid=2293420
target_start_affinity tid=2293420 cpus=8-13
pid 2293420's current scheduling policy: SCHED_OTHER
pid 2293420's current scheduling priority: 0
target_start_tid=2293421
target_start_affinity tid=2293421 cpus=8-13
pid 2293421's current scheduling policy: SCHED_OTHER
pid 2293421's current scheduling priority: 0
target_start_tid=2293591
target_start_affinity tid=2293591 cpus=8-13
pid 2293591's current scheduling policy: SCHED_OTHER
pid 2293591's current scheduling priority: 0
target_start_tid=2293592
target_start_affinity tid=2293592 cpus=8-13
pid 2293592's current scheduling policy: SCHED_OTHER
pid 2293592's current scheduling priority: 0
target_start_tid=2293593
target_start_affinity tid=2293593 cpus=8-13
pid 2293593's current scheduling policy: SCHED_OTHER
pid 2293593's current scheduling priority: 0
target_start_tid=2293594
target_start_affinity tid=2293594 cpus=8-13
pid 2293594's current scheduling policy: SCHED_OTHER
pid 2293594's current scheduling priority: 0
target_start_tid=2293599
target_start_affinity tid=2293599 cpus=8-13
pid 2293599's current scheduling policy: SCHED_OTHER
pid 2293599's current scheduling priority: 0
target_start_tid=2293600
target_start_affinity tid=2293600 cpus=8-13
pid 2293600's current scheduling policy: SCHED_OTHER
pid 2293600's current scheduling priority: 0
target_start_tid=2293604
target_start_affinity tid=2293604 cpus=8-13
pid 2293604's current scheduling policy: SCHED_OTHER
pid 2293604's current scheduling priority: 0
target_start_process_pid=2293603
target_start_tid=2293603
target_start_affinity tid=2293603 cpus=8-13
pid 2293603's current scheduling policy: SCHED_OTHER
pid 2293603's current scheduling priority: 0
target_start_tid=2293605
target_start_affinity tid=2293605 cpus=8-13
pid 2293605's current scheduling policy: SCHED_OTHER
pid 2293605's current scheduling priority: 0
target_start_tid=2293606
target_start_affinity tid=2293606 cpus=8-13
pid 2293606's current scheduling policy: SCHED_OTHER
pid 2293606's current scheduling priority: 0
target_start_tid=2293607
target_start_affinity tid=2293607 cpus=8-13
pid 2293607's current scheduling policy: SCHED_OTHER
pid 2293607's current scheduling priority: 0
target_start_tid=2293608
target_start_affinity tid=2293608 cpus=8-13
pid 2293608's current scheduling policy: SCHED_OTHER
pid 2293608's current scheduling priority: 0
target_start_tid=2293609
target_start_affinity tid=2293609 cpus=8-13
pid 2293609's current scheduling policy: SCHED_OTHER
pid 2293609's current scheduling priority: 0
target_start_tid=2293610
target_start_affinity tid=2293610 cpus=8-13
pid 2293610's current scheduling policy: SCHED_OTHER
pid 2293610's current scheduling priority: 0
target_start_tid=2293612
target_start_affinity tid=2293612 cpus=8
pid 2293612's current scheduling policy: SCHED_OTHER
pid 2293612's current scheduling priority: 0
target_start_tid=2293613
target_start_affinity tid=2293613 cpus=9
pid 2293613's current scheduling policy: SCHED_OTHER
pid 2293613's current scheduling priority: 0
target_start_tid=2293614
target_start_affinity tid=2293614 cpus=10
pid 2293614's current scheduling policy: SCHED_OTHER
pid 2293614's current scheduling priority: 0
target_start_tid=2293615
target_start_affinity tid=2293615 cpus=11
pid 2293615's current scheduling policy: SCHED_OTHER
pid 2293615's current scheduling priority: 0
target_start_tid=2293616
target_start_affinity tid=2293616 cpus=12
pid 2293616's current scheduling policy: SCHED_OTHER
pid 2293616's current scheduling priority: 0
target_start_tid=2293617
target_start_affinity tid=2293617 cpus=13
pid 2293617's current scheduling policy: SCHED_OTHER
pid 2293617's current scheduling priority: 0
target_start_tid=2293618
target_start_affinity tid=2293618 cpus=8-13
pid 2293618's current scheduling policy: SCHED_OTHER
pid 2293618's current scheduling priority: 0
target_start_tid=2293622
target_start_affinity tid=2293622 cpus=8-13
pid 2293622's current scheduling policy: SCHED_OTHER
pid 2293622's current scheduling priority: 0
target_start_tid=2293623
target_start_affinity tid=2293623 cpus=8-13
pid 2293623's current scheduling policy: SCHED_OTHER
pid 2293623's current scheduling priority: 0
target_start_tid=2293624
target_start_affinity tid=2293624 cpus=8-13
pid 2293624's current scheduling policy: SCHED_OTHER
pid 2293624's current scheduling priority: 0
target_start_tid=2293628
target_start_affinity tid=2293628 cpus=8-13
pid 2293628's current scheduling policy: SCHED_OTHER
pid 2293628's current scheduling priority: 0
target_start_tid=2293629
target_start_affinity tid=2293629 cpus=8-13
pid 2293629's current scheduling policy: SCHED_OTHER
pid 2293629's current scheduling priority: 0
target_start_tid=2293630
target_start_affinity tid=2293630 cpus=8-13
pid 2293630's current scheduling policy: SCHED_OTHER
pid 2293630's current scheduling priority: 0
target_start_tid=2293631
target_start_affinity tid=2293631 cpus=8-13
pid 2293631's current scheduling policy: SCHED_OTHER
pid 2293631's current scheduling priority: 0
target_start_tid=2293632
target_start_affinity tid=2293632 cpus=8-13
pid 2293632's current scheduling policy: SCHED_OTHER
pid 2293632's current scheduling priority: 0
target_start_tid=2293633
target_start_affinity tid=2293633 cpus=8-13
pid 2293633's current scheduling policy: SCHED_OTHER
pid 2293633's current scheduling priority: 0
target_start_tid=2293641
target_start_affinity tid=2293641 cpus=8-13
pid 2293641's current scheduling policy: SCHED_OTHER
pid 2293641's current scheduling priority: 0
target_start_tid=2293642
target_start_affinity tid=2293642 cpus=8-13
pid 2293642's current scheduling policy: SCHED_OTHER
pid 2293642's current scheduling priority: 0
target_start_tid=2293643
target_start_affinity tid=2293643 cpus=8-13
pid 2293643's current scheduling policy: SCHED_OTHER
pid 2293643's current scheduling priority: 0
target_start_tid=2293644
target_start_affinity tid=2293644 cpus=8-13
pid 2293644's current scheduling policy: SCHED_OTHER
pid 2293644's current scheduling priority: 0
target_start_tid=2293648
target_start_affinity tid=2293648 cpus=8-13
pid 2293648's current scheduling policy: SCHED_OTHER
pid 2293648's current scheduling priority: 0
target_start_tid=2293649
target_start_affinity tid=2293649 cpus=8-13
pid 2293649's current scheduling policy: SCHED_OTHER
pid 2293649's current scheduling priority: 0
target_start_tid=2293650
target_start_affinity tid=2293650 cpus=8-13
pid 2293650's current scheduling policy: SCHED_OTHER
pid 2293650's current scheduling priority: 0
target_start_tid=2293657
target_start_affinity tid=2293657 cpus=8-13
pid 2293657's current scheduling policy: SCHED_OTHER
pid 2293657's current scheduling priority: 0
target_start_tid=2293658
target_start_affinity tid=2293658 cpus=8-13
pid 2293658's current scheduling policy: SCHED_OTHER
pid 2293658's current scheduling priority: 0

mid_active_source_utc=2026-11-14T00:14:49Z
mid_active_source_pid=2293659
mid_active_source_process_pid=2293659
mid_active_source_tid=2293659
mid_active_source_affinity tid=2293659 cpus=0-5
pid 2293659's current scheduling policy: SCHED_OTHER
pid 2293659's current scheduling priority: 0
mid_active_source_tid=2293663
mid_active_source_affinity tid=2293663 cpus=0-5
pid 2293663's current scheduling policy: SCHED_OTHER
pid 2293663's current scheduling priority: 0
mid_active_source_tid=2293664
mid_active_source_affinity tid=2293664 cpus=0-5
pid 2293664's current scheduling policy: SCHED_OTHER
pid 2293664's current scheduling priority: 0
mid_active_source_tid=2293665
mid_active_source_affinity tid=2293665 cpus=0-5
pid 2293665's current scheduling policy: SCHED_OTHER
pid 2293665's current scheduling priority: 0
mid_active_source_tid=2293666
mid_active_source_affinity tid=2293666 cpus=0-5
pid 2293666's current scheduling policy: SCHED_OTHER
pid 2293666's current scheduling priority: 0
mid_active_source_tid=2293667
mid_active_source_affinity tid=2293667 cpus=0-5
pid 2293667's current scheduling policy: SCHED_OTHER
pid 2293667's current scheduling priority: 0
mid_active_source_tid=2293668
mid_active_source_affinity tid=2293668 cpus=0-5
pid 2293668's current scheduling policy: SCHED_OTHER
pid 2293668's current scheduling priority: 0
mid_active_source_tid=2293669
mid_active_source_affinity tid=2293669 cpus=0-5
pid 2293669's current scheduling policy: SCHED_OTHER
pid 2293669's current scheduling priority: 0
mid_active_source_tid=2293671
mid_active_source_affinity tid=2293671 cpus=0-5
pid 2293671's current scheduling policy: SCHED_OTHER
pid 2293671's current scheduling priority: 0
mid_active_source_tid=2293715
mid_active_source_affinity tid=2293715 cpus=0-5
pid 2293715's current scheduling policy: SCHED_OTHER
pid 2293715's current scheduling priority: 0
mid_active_source_tid=2293716
mid_active_source_affinity tid=2293716 cpus=0-5
pid 2293716's current scheduling policy: SCHED_OTHER
pid 2293716's current scheduling priority: 0
mid_active_source_tid=2293717
mid_active_source_affinity tid=2293717 cpus=0-5
pid 2293717's current scheduling policy: SCHED_OTHER
pid 2293717's current scheduling priority: 0
mid_active_source_tid=2293719
mid_active_source_affinity tid=2293719 cpus=0-5
pid 2293719's current scheduling policy: SCHED_OTHER
pid 2293719's current scheduling priority: 0
mid_active_source_tid=2293756
mid_active_source_affinity tid=2293756 cpus=0-5
pid 2293756's current scheduling policy: SCHED_OTHER
pid 2293756's current scheduling priority: 0
mid_active_source_tid=2293759
mid_active_source_affinity tid=2293759 cpus=0-5
pid 2293759's current scheduling policy: SCHED_OTHER
pid 2293759's current scheduling priority: 0
mid_active_source_tid=2293773
mid_active_source_affinity tid=2293773 cpus=0-5
pid 2293773's current scheduling policy: SCHED_OTHER
pid 2293773's current scheduling priority: 0
mid_active_source_process_pid=2293771
mid_active_source_tid=2293771
mid_active_source_affinity tid=2293771 cpus=0-5
pid 2293771's current scheduling policy: SCHED_OTHER
pid 2293771's current scheduling priority: 0
mid_active_source_tid=2293777
mid_active_source_affinity tid=2293777 cpus=0-5
pid 2293777's current scheduling policy: SCHED_OTHER
pid 2293777's current scheduling priority: 0
mid_active_source_tid=2293778
mid_active_source_affinity tid=2293778 cpus=0-5
pid 2293778's current scheduling policy: SCHED_OTHER
pid 2293778's current scheduling priority: 0
mid_active_source_tid=2293779
mid_active_source_affinity tid=2293779 cpus=0-5
pid 2293779's current scheduling policy: SCHED_OTHER
pid 2293779's current scheduling priority: 0
mid_active_source_tid=2293781
mid_active_source_affinity tid=2293781 cpus=0-5
pid 2293781's current scheduling policy: SCHED_OTHER
pid 2293781's current scheduling priority: 0
mid_active_source_tid=2293783
mid_active_source_affinity tid=2293783 cpus=0-5
pid 2293783's current scheduling policy: SCHED_OTHER
pid 2293783's current scheduling priority: 0
mid_active_source_tid=2293784
mid_active_source_affinity tid=2293784 cpus=0-5
pid 2293784's current scheduling policy: SCHED_OTHER
pid 2293784's current scheduling priority: 0
mid_active_source_tid=2293785
mid_active_source_affinity tid=2293785 cpus=0-5
pid 2293785's current scheduling policy: SCHED_OTHER
pid 2293785's current scheduling priority: 0
mid_active_source_tid=2293793
mid_active_source_affinity tid=2293793 cpus=0-5
pid 2293793's current scheduling policy: SCHED_OTHER
pid 2293793's current scheduling priority: 0
mid_active_source_tid=2293815
mid_active_source_affinity tid=2293815 cpus=0-5
pid 2293815's current scheduling policy: SCHED_OTHER
pid 2293815's current scheduling priority: 0
mid_active_source_tid=2293816
mid_active_source_affinity tid=2293816 cpus=0-5
pid 2293816's current scheduling policy: SCHED_OTHER
pid 2293816's current scheduling priority: 0
mid_active_source_tid=2293817
mid_active_source_affinity tid=2293817 cpus=0-5
pid 2293817's current scheduling policy: SCHED_OTHER
pid 2293817's current scheduling priority: 0
mid_active_source_tid=2293818
mid_active_source_affinity tid=2293818 cpus=0-5
pid 2293818's current scheduling policy: SCHED_OTHER
pid 2293818's current scheduling priority: 0
mid_active_source_tid=2293819
mid_active_source_affinity tid=2293819 cpus=0-5
pid 2293819's current scheduling policy: SCHED_OTHER
pid 2293819's current scheduling priority: 0
mid_active_source_tid=2293820
mid_active_source_affinity tid=2293820 cpus=0-5
pid 2293820's current scheduling policy: SCHED_OTHER
pid 2293820's current scheduling priority: 0
mid_active_source_tid=2293825
mid_active_source_affinity tid=2293825 cpus=0-5
pid 2293825's current scheduling policy: SCHED_OTHER
pid 2293825's current scheduling priority: 0
mid_active_source_tid=2293838
mid_active_source_affinity tid=2293838 cpus=0-5
pid 2293838's current scheduling policy: SCHED_OTHER
pid 2293838's current scheduling priority: 0
mid_active_source_tid=2293851
mid_active_source_affinity tid=2293851 cpus=0-5
pid 2293851's current scheduling policy: SCHED_OTHER
pid 2293851's current scheduling priority: 0
mid_active_source_tid=2293852
mid_active_source_affinity tid=2293852 cpus=0-5
pid 2293852's current scheduling policy: SCHED_OTHER
pid 2293852's current scheduling priority: 0
mid_active_source_tid=2293853
mid_active_source_affinity tid=2293853 cpus=0-5
pid 2293853's current scheduling policy: SCHED_OTHER
pid 2293853's current scheduling priority: 0
mid_active_source_tid=2293905
mid_active_source_affinity tid=2293905 cpus=0-5
pid 2293905's current scheduling policy: SCHED_OTHER
pid 2293905's current scheduling priority: 0
mid_active_source_tid=2293913
mid_active_source_affinity tid=2293913 cpus=0-5
pid 2293913's current scheduling policy: SCHED_OTHER
pid 2293913's current scheduling priority: 0
mid_active_source_tid=2293915
mid_active_source_affinity tid=2293915 cpus=0-5
pid 2293915's current scheduling policy: SCHED_OTHER
pid 2293915's current scheduling priority: 0
mid_active_source_tid=2293919
mid_active_source_affinity tid=2293919 cpus=0-5
pid 2293919's current scheduling policy: SCHED_OTHER
pid 2293919's current scheduling priority: 0
mid_active_source_tid=2293922
mid_active_source_affinity tid=2293922 cpus=0-5
pid 2293922's current scheduling policy: SCHED_OTHER
pid 2293922's current scheduling priority: 0
mid_active_source_tid=2293924
mid_active_source_affinity tid=2293924 cpus=0-5
pid 2293924's current scheduling policy: SCHED_OTHER
pid 2293924's current scheduling priority: 0
mid_active_source_tid=2293974
mid_active_source_affinity tid=2293974 cpus=0-5
pid 2293974's current scheduling policy: SCHED_OTHER
pid 2293974's current scheduling priority: 0
mid_active_source_tid=2293975
mid_active_source_affinity tid=2293975 cpus=0-5
pid 2293975's current scheduling policy: SCHED_OTHER
pid 2293975's current scheduling priority: 0
mid_active_source_tid=2294741
mid_active_source_affinity tid=2294741 cpus=0-5
pid 2294741's current scheduling policy: SCHED_OTHER
pid 2294741's current scheduling priority: 0
mid_active_source_tid=2294745
mid_active_source_affinity tid=2294745 cpus=0-5
pid 2294745's current scheduling policy: SCHED_OTHER
pid 2294745's current scheduling priority: 0
mid_active_source_tid=2294918
mid_active_source_affinity tid=2294918 cpus=0-5
pid 2294918's current scheduling policy: SCHED_OTHER
pid 2294918's current scheduling priority: 0
mid_active_source_tid=2296301
mid_active_source_affinity tid=2296301 cpus=0-5
pid 2296301's current scheduling policy: SCHED_OTHER
pid 2296301's current scheduling priority: 0
mid_active_source_tid=2296303
mid_active_source_affinity tid=2296303 cpus=0-5
pid 2296303's current scheduling policy: SCHED_OTHER
pid 2296303's current scheduling priority: 0

mid_active_target_utc=2026-11-14T00:14:50Z
mid_active_target_pid=2293406
mid_active_target_process_pid=2293406
mid_active_target_tid=2293406
mid_active_target_affinity tid=2293406 cpus=8-13
pid 2293406's current scheduling policy: SCHED_OTHER
pid 2293406's current scheduling priority: 0
mid_active_target_tid=2293412
mid_active_target_affinity tid=2293412 cpus=8-13
pid 2293412's current scheduling policy: SCHED_OTHER
pid 2293412's current scheduling priority: 0
mid_active_target_tid=2293413
mid_active_target_affinity tid=2293413 cpus=8-13
pid 2293413's current scheduling policy: SCHED_OTHER
pid 2293413's current scheduling priority: 0
mid_active_target_tid=2293415
mid_active_target_affinity tid=2293415 cpus=8-13
pid 2293415's current scheduling policy: SCHED_OTHER
pid 2293415's current scheduling priority: 0
mid_active_target_tid=2293416
mid_active_target_affinity tid=2293416 cpus=8-13
pid 2293416's current scheduling policy: SCHED_OTHER
pid 2293416's current scheduling priority: 0
mid_active_target_tid=2293417
mid_active_target_affinity tid=2293417 cpus=8-13
pid 2293417's current scheduling policy: SCHED_OTHER
pid 2293417's current scheduling priority: 0
mid_active_target_tid=2293418
mid_active_target_affinity tid=2293418 cpus=8-13
pid 2293418's current scheduling policy: SCHED_OTHER
pid 2293418's current scheduling priority: 0
mid_active_target_tid=2293419
mid_active_target_affinity tid=2293419 cpus=8-13
pid 2293419's current scheduling policy: SCHED_OTHER
pid 2293419's current scheduling priority: 0
mid_active_target_tid=2293421
mid_active_target_affinity tid=2293421 cpus=8-13
pid 2293421's current scheduling policy: SCHED_OTHER
pid 2293421's current scheduling priority: 0
mid_active_target_tid=2293591
mid_active_target_affinity tid=2293591 cpus=8-13
pid 2293591's current scheduling policy: SCHED_OTHER
pid 2293591's current scheduling priority: 0
mid_active_target_tid=2293592
mid_active_target_affinity tid=2293592 cpus=8-13
pid 2293592's current scheduling policy: SCHED_OTHER
pid 2293592's current scheduling priority: 0
mid_active_target_tid=2293593
mid_active_target_affinity tid=2293593 cpus=8-13
pid 2293593's current scheduling policy: SCHED_OTHER
pid 2293593's current scheduling priority: 0
mid_active_target_tid=2293594
mid_active_target_affinity tid=2293594 cpus=8-13
pid 2293594's current scheduling policy: SCHED_OTHER
pid 2293594's current scheduling priority: 0
mid_active_target_tid=2293599
mid_active_target_affinity tid=2293599 cpus=8-13
pid 2293599's current scheduling policy: SCHED_OTHER
pid 2293599's current scheduling priority: 0
mid_active_target_tid=2293600
mid_active_target_affinity tid=2293600 cpus=8-13
pid 2293600's current scheduling policy: SCHED_OTHER
pid 2293600's current scheduling priority: 0
mid_active_target_tid=2293604
mid_active_target_affinity tid=2293604 cpus=8-13
pid 2293604's current scheduling policy: SCHED_OTHER
pid 2293604's current scheduling priority: 0
mid_active_target_process_pid=2293603
mid_active_target_tid=2293603
mid_active_target_affinity tid=2293603 cpus=8-13
pid 2293603's current scheduling policy: SCHED_OTHER
pid 2293603's current scheduling priority: 0
mid_active_target_tid=2293605
mid_active_target_affinity tid=2293605 cpus=8-13
pid 2293605's current scheduling policy: SCHED_OTHER
pid 2293605's current scheduling priority: 0
mid_active_target_tid=2293606
mid_active_target_affinity tid=2293606 cpus=8-13
pid 2293606's current scheduling policy: SCHED_OTHER
pid 2293606's current scheduling priority: 0
mid_active_target_tid=2293607
mid_active_target_affinity tid=2293607 cpus=8-13
pid 2293607's current scheduling policy: SCHED_OTHER
pid 2293607's current scheduling priority: 0
mid_active_target_tid=2293608
mid_active_target_affinity tid=2293608 cpus=8-13
pid 2293608's current scheduling policy: SCHED_OTHER
pid 2293608's current scheduling priority: 0
mid_active_target_tid=2293609
mid_active_target_affinity tid=2293609 cpus=8-13
pid 2293609's current scheduling policy: SCHED_OTHER
pid 2293609's current scheduling priority: 0
mid_active_target_tid=2293610
mid_active_target_affinity tid=2293610 cpus=8-13
pid 2293610's current scheduling policy: SCHED_OTHER
pid 2293610's current scheduling priority: 0
mid_active_target_tid=2293612
mid_active_target_affinity tid=2293612 cpus=8
pid 2293612's current scheduling policy: SCHED_OTHER
pid 2293612's current scheduling priority: 0
mid_active_target_tid=2293613
mid_active_target_affinity tid=2293613 cpus=9
pid 2293613's current scheduling policy: SCHED_OTHER
pid 2293613's current scheduling priority: 0
mid_active_target_tid=2293614
mid_active_target_affinity tid=2293614 cpus=10
pid 2293614's current scheduling policy: SCHED_OTHER
pid 2293614's current scheduling priority: 0
mid_active_target_tid=2293615
mid_active_target_affinity tid=2293615 cpus=11
pid 2293615's current scheduling policy: SCHED_OTHER
pid 2293615's current scheduling priority: 0
mid_active_target_tid=2293616
mid_active_target_affinity tid=2293616 cpus=12
pid 2293616's current scheduling policy: SCHED_OTHER
pid 2293616's current scheduling priority: 0
mid_active_target_tid=2293617
mid_active_target_affinity tid=2293617 cpus=13
pid 2293617's current scheduling policy: SCHED_OTHER
pid 2293617's current scheduling priority: 0
mid_active_target_tid=2293618
mid_active_target_affinity tid=2293618 cpus=8-13
pid 2293618's current scheduling policy: SCHED_OTHER
pid 2293618's current scheduling priority: 0
mid_active_target_tid=2293622
mid_active_target_affinity tid=2293622 cpus=8-13
pid 2293622's current scheduling policy: SCHED_OTHER
pid 2293622's current scheduling priority: 0
mid_active_target_tid=2293623
mid_active_target_affinity tid=2293623 cpus=8-13
pid 2293623's current scheduling policy: SCHED_OTHER
pid 2293623's current scheduling priority: 0
mid_active_target_tid=2293624
mid_active_target_affinity tid=2293624 cpus=8-13
pid 2293624's current scheduling policy: SCHED_OTHER
pid 2293624's current scheduling priority: 0
mid_active_target_tid=2293628
mid_active_target_affinity tid=2293628 cpus=8-13
pid 2293628's current scheduling policy: SCHED_OTHER
pid 2293628's current scheduling priority: 0
mid_active_target_tid=2293629
mid_active_target_affinity tid=2293629 cpus=8-13
pid 2293629's current scheduling policy: SCHED_OTHER
pid 2293629's current scheduling priority: 0
mid_active_target_tid=2293630
mid_active_target_affinity tid=2293630 cpus=8-13
pid 2293630's current scheduling policy: SCHED_OTHER
pid 2293630's current scheduling priority: 0
mid_active_target_tid=2293631
mid_active_target_affinity tid=2293631 cpus=8-13
pid 2293631's current scheduling policy: SCHED_OTHER
pid 2293631's current scheduling priority: 0
mid_active_target_tid=2293632
mid_active_target_affinity tid=2293632 cpus=8-13
pid 2293632's current scheduling policy: SCHED_OTHER
pid 2293632's current scheduling priority: 0
mid_active_target_tid=2293633
mid_active_target_affinity tid=2293633 cpus=8-13
pid 2293633's current scheduling policy: SCHED_OTHER
pid 2293633's current scheduling priority: 0
mid_active_target_tid=2293641
mid_active_target_affinity tid=2293641 cpus=8-13
pid 2293641's current scheduling policy: SCHED_OTHER
pid 2293641's current scheduling priority: 0
mid_active_target_tid=2293642
mid_active_target_affinity tid=2293642 cpus=8-13
pid 2293642's current scheduling policy: SCHED_OTHER
pid 2293642's current scheduling priority: 0
mid_active_target_tid=2293643
mid_active_target_affinity tid=2293643 cpus=8-13
pid 2293643's current scheduling policy: SCHED_OTHER
pid 2293643's current scheduling priority: 0
mid_active_target_tid=2293644
mid_active_target_affinity tid=2293644 cpus=8-13
pid 2293644's current scheduling policy: SCHED_OTHER
pid 2293644's current scheduling priority: 0
mid_active_target_tid=2293648
mid_active_target_affinity tid=2293648 cpus=8-13
pid 2293648's current scheduling policy: SCHED_OTHER
pid 2293648's current scheduling priority: 0
mid_active_target_tid=2293649
mid_active_target_affinity tid=2293649 cpus=8-13
pid 2293649's current scheduling policy: SCHED_OTHER
pid 2293649's current scheduling priority: 0
mid_active_target_tid=2293650
mid_active_target_affinity tid=2293650 cpus=8-13
pid 2293650's current scheduling policy: SCHED_OTHER
pid 2293650's current scheduling priority: 0
mid_active_target_tid=2293657
mid_active_target_affinity tid=2293657 cpus=8-13
pid 2293657's current scheduling policy: SCHED_OTHER
pid 2293657's current scheduling priority: 0
mid_active_target_tid=2293658
mid_active_target_affinity tid=2293658 cpus=8-13
pid 2293658's current scheduling policy: SCHED_OTHER
pid 2293658's current scheduling priority: 0
mid_active_target_tid=2296235
mid_active_target_affinity tid=2296235 cpus=8-13
pid 2296235's current scheduling policy: SCHED_OTHER
pid 2296235's current scheduling priority: 0 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-serial-1024/warm/attempt3/run1/zlink-dotnet-request-serial-1024/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-serial-1024/warm/attempt3/run1/zlink-dotnet-request-serial-1024/scheduling.txt
- 2026-11-14T00:14:54Z level=L1 cell=request-serial-1024 stage=warm HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding; metrics: same target record; copies: protobuf body.
- repeat level=L1 cell=request-serial-1024 stage=warm attempt=4 rc=0 output=7958.4 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-serial-1024/warm/attempt4/run1/zlink-dotnet-request-serial-1024/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-serial-1024/warm/attempt4/run1/zlink-dotnet-request-serial-1024/scheduling.txt
- 2026-11-14T00:15:14Z level=L1 cell=request-serial-1024 stage=counted HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding; metrics: same target record; copies: protobuf body.
- repeat level=L1 cell=request-serial-1024 stage=counted attempt=2 rc=0 output=7940 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-serial-1024/counted/attempt2/run1/zlink-dotnet-request-serial-1024/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-serial-1024/counted/attempt2/run1/zlink-dotnet-request-serial-1024/scheduling.txt
- 2026-11-14T00:15:34Z level=L1 cell=request-serial-1024 stage=counted HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding; metrics: same target record; copies: protobuf body.
- repeat level=L1 cell=request-serial-1024 stage=counted attempt=3 rc=0 output=7979.2 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-serial-1024/counted/attempt3/run1/zlink-dotnet-request-serial-1024/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-serial-1024/counted/attempt3/run1/zlink-dotnet-request-serial-1024/scheduling.txt
- 2026-11-14T00:15:54Z level=L2 cell=request-serial-1024 stage=warm HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding; metrics: same target record; copies: protobuf body.
- repeat level=L2 cell=request-serial-1024 stage=warm attempt=2 rc=0 output=7515.2 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-serial-1024/warm/attempt2/run1/zlink-dotnet-request-serial-1024/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-serial-1024/warm/attempt2/run1/zlink-dotnet-request-serial-1024/scheduling.txt
- 2026-11-14T00:16:14Z level=L2 cell=request-serial-1024 stage=counted HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding; metrics: same target record; copies: protobuf body.
- repeat level=L2 cell=request-serial-1024 stage=counted attempt=2 rc=0 output=7400 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-serial-1024/counted/attempt2/run1/zlink-dotnet-request-serial-1024/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-serial-1024/counted/attempt2/run1/zlink-dotnet-request-serial-1024/scheduling.txt
- 2026-11-14T00:16:34Z level=L2 cell=request-serial-1024 stage=counted HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding; metrics: same target record; copies: protobuf body.
- repeat level=L2 cell=request-serial-1024 stage=counted attempt=3 rc=0 output=7376 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-serial-1024/counted/attempt3/run1/zlink-dotnet-request-serial-1024/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-serial-1024/counted/attempt3/run1/zlink-dotnet-request-serial-1024/scheduling.txt
- 2026-11-14T00:16:54Z level=L1 cell=request-serial-4096 stage=warm HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding; metrics: same target record; copies: protobuf body.
- repeat level=L1 cell=request-serial-4096 stage=warm attempt=2 rc=0 output=7968.4 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-serial-4096/warm/attempt2/run1/zlink-dotnet-request-serial-4096/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-serial-4096/warm/attempt2/run1/zlink-dotnet-request-serial-4096/scheduling.txt
- 2026-11-14T00:17:14Z level=L1 cell=request-serial-4096 stage=counted HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding; metrics: same target record; copies: protobuf body.
- repeat level=L1 cell=request-serial-4096 stage=counted attempt=2 rc=0 output=7455.8 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-serial-4096/counted/attempt2/run1/zlink-dotnet-request-serial-4096/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-serial-4096/counted/attempt2/run1/zlink-dotnet-request-serial-4096/scheduling.txt
- 2026-11-14T00:17:35Z level=L1 cell=request-serial-4096 stage=counted HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding; metrics: same target record; copies: protobuf body.
- repeat level=L1 cell=request-serial-4096 stage=counted attempt=3 rc=0 output=7427.2 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-serial-4096/counted/attempt3/run1/zlink-dotnet-request-serial-4096/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-serial-4096/counted/attempt3/run1/zlink-dotnet-request-serial-4096/scheduling.txt
- 2026-11-14T00:17:55Z level=L2 cell=request-serial-4096 stage=warm HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding; metrics: same target record; copies: protobuf body.
- repeat level=L2 cell=request-serial-4096 stage=warm attempt=2 rc=0 output=7054 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-serial-4096/warm/attempt2/run1/zlink-dotnet-request-serial-4096/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-serial-4096/warm/attempt2/run1/zlink-dotnet-request-serial-4096/scheduling.txt
- 2026-11-14T00:18:15Z level=L2 cell=request-serial-4096 stage=counted HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding; metrics: same target record; copies: protobuf body.
- repeat level=L2 cell=request-serial-4096 stage=counted attempt=2 rc=0 output=6961.4 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-serial-4096/counted/attempt2/run1/zlink-dotnet-request-serial-4096/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-serial-4096/counted/attempt2/run1/zlink-dotnet-request-serial-4096/scheduling.txt
- 2026-11-14T00:18:35Z level=L2 cell=request-serial-4096 stage=counted HEAD=4bd6102170ad5ff405cab7bccc74113aa71b4ea1 dirty=clean merge-base=6350275facc547759c9e143cc2515f3f528c776e artifacts=framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll@2026-11-14 08:32:13.405399252 +0900#35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723;framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll@2026-11-14 08:32:13.440397824 +0900#f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332;framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll@2026-11-14 08:31:41.973682267 +0900#6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552
  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding; metrics: same target record; copies: protobuf body.
- repeat level=L2 cell=request-serial-4096 stage=counted attempt=3 rc=0 output=7095.4 raw=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-serial-4096/counted/attempt3/run1/zlink-dotnet-request-serial-4096/results.json scheduling=/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-serial-4096/counted/attempt3/run1/zlink-dotnet-request-serial-4096/scheduling.txt
- L2 vs L1 완료 판정: 4개 단일-run 셀 모두 PASS; request-serial 1K median L1=7,940, L2=7,376, loss=7.10% FAIL (G5 L1=3.17%, L2=3.83%); request-serial 4K median L1=7,427.2, L2=6,961.4, loss=6.27%, 그러나 L2 G5=5.01%로 판정 불가. 따라서 L2에서 STOP; L3/L4 미측정.
- L2 추가 비용은 request·response envelope header encode/parse; profile은 managed symbol 미해석으로 parser 단위 attribution에는 한계가 있음.
- Boundary valid-repeat hold (00:09:51–00:18:55 UTC, wait included; measurement window under 10 min) rc=0. Counted valid attempts 2–3로 3개씩 채움. invalid warm attempts는 보존·제외.
- 보조 L2 request-serial 1K profile (no lock, 15초, CPU 6–7/14–15): successful run raw [perf.data](/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/profile-L2-request-serial-1024-final/perf.data), 4,613 samples, lost=0; profile runner result는 counted verdict에 넣지 않음. [scheduling](/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/profile-L2-request-serial-1024-final/bench/run1/zlink-dotnet-request-serial-1024/scheduling.txt). Managed application symbols가 해석되지 않아 parser 단위 비용은 분리되지 않음. 실패한 준비 profile raw도 모두 보존.
