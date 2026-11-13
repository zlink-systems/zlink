# 진행 기록

- 2026-11-13 23:27 UTC: 현재 worktree branch `exp/4350-dotnet-fixes`, HEAD `4bd6102170ad5ff405cab7bccc74113aa71b4ea1`, merge-base `6350275facc547759c9e143cc2515f3f528c776e`, 시작 상태 clean. ladder bench commit 포함.
- 측정 전 call-shape: [call-shape.md](/root/jobs/fx/cx/v21/s033/lfx-dotnet/call-shape.md). binding/L1/L2/L3/L4의 send·receive·reply·completion 경로와 단계별 추가를 비교했다.
- runner는 lad-dotnet-s1 도구를 이 job 디렉터리로 복사하고 worktree/job 경로를 현재 경로로 변경했다.
- Release L5_7-compatible build 실행 중: [release-build.log](/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/release-build.log). 완료 후 rc, 소스보다 신규인 artifact, SHA256을 기록하고 모든 측정 전 progress에 다시 쓴다.
- Release build 재시도 성공(rc=0): [release-build-friend-target.log](/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/release-build-friend-target.log). 외부 `/tmp/lfx-dotnet-ladder-bench-friends.targets`가 bench friend assembly metadata만 Framework build에 제공했고, worktree는 계속 clean. artifacts는 Client `35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723`, ZLinkServer `f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332`, ZLinkRawServer `6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552`; 모두 관련 `.cs/.props/.csproj`보다 새로 생성됨.
- 측정 전 hold 검사(23:36 UTC): 다른 worktree의 `dotnet build`, CMake/Ninja 및 test/build 작업이 R/S 상태여서 common-rules의 BUILDS BETWEEN HOLDS에 따라 lock을 아직 취하지 않음. 시작 후 10분 이내 완료 여부를 확인한다.
