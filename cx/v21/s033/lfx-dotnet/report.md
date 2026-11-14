# .NET ladder fix-branch 측정

- branch `exp/4350-dotnet-fixes`, HEAD `4bd6102170ad5ff405cab7bccc74113aa71b4ea1`, merge-base `6350275facc547759c9e143cc2515f3f528c776e`.
- 조건: 2026-11-13 UTC, warmup 10초/active 5초, source 0–5 / target 8–13, 모든 thread SCHED_OTHER 및 start·mid-active affinity 기록. 각 counted run errors=0, submitted=completed=received.
- Build: Release L5_7-compatible bench, rc=0. 현재 fix branch에는 ladder friend assembly metadata가 없어 외부 임시 MSBuild target으로 bench assemblies에만 제공했다. worktree 변경 없음. Artifact SHA256: Client `35072dd40fe335adbe152ae4b99b679ad53067c3c9d9fa3e27e57beec0d1f723`; ZLinkServer `f4080c4b65b57b6784480cd5e53e498043931e6962d38883fde66de5a771e332`; ZLinkRawServer `6313f934e571bd9758dcfc9a7d9e846139a75ce561283e59b0a79fba83e83552`. 세 artifact는 대응 source보다 새롭다. 상세 build log: [release-build-friend-target.log](/root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/release-build-friend-target.log).

## L2 vs L1 — 경계 셀 반복 중

| 셀 | L1 msg/s | L2 msg/s | 손실 | 판정 | raw |
|---|---:|---:|---:|---|---|
| send-saturation · 1K | 358,206.8 | 343,949.6 | 3.98% | PASS | [L1](</root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/send-saturation-1024/counted/attempt1/run1/zlink-dotnet-send-saturation-1024/results.json>) · [L2](</root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/send-saturation-1024/counted/attempt1/run1/zlink-dotnet-send-saturation-1024/results.json>) |
| send-saturation · 4K | 148,668.6 | 145,166.0 | 2.36% | PASS | [L1](</root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/send-saturation-4096/counted/attempt1/run1/zlink-dotnet-send-saturation-4096/results.json>) · [L2](</root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/send-saturation-4096/counted/attempt1/run1/zlink-dotnet-send-saturation-4096/results.json>) |
| request-serial · 1K | 7,688.0 | 7,093.2 | 7.74% | 경계: 반복 중 | [L1](</root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-serial-1024/counted/attempt1/run1/zlink-dotnet-request-serial-1024/results.json>) · [L2](</root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-serial-1024/counted/attempt1/run1/zlink-dotnet-request-serial-1024/results.json>) |
| request-serial · 4K | 7,204.4 | 6,612.6 | 8.21% | 경계: 반복 중 | [L1](</root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-serial-4096/counted/attempt1/run1/zlink-dotnet-request-serial-4096/results.json>) · [L2](</root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-serial-4096/counted/attempt1/run1/zlink-dotnet-request-serial-4096/results.json>) |
| request-backpressure · 1K | 66,499.2 | 65,142.6 | 2.04% | PASS | [L1](</root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-backpressure-1024/counted/attempt1/run1/zlink-dotnet-request-backpressure-1024/results.json>) · [L2](</root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-backpressure-1024/counted/attempt1/run1/zlink-dotnet-request-backpressure-1024/results.json>) |
| request-backpressure · 4K | 71,681.6 | 70,038.8 | 2.29% | PASS | [L1](</root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L1/request-backpressure-4096/counted/attempt1/run1/zlink-dotnet-request-backpressure-4096/results.json>) · [L2](</root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/L2/request-backpressure-4096/counted/attempt1/run1/zlink-dotnet-request-backpressure-4096/results.json>) |

- Hold L1/L2: [window](</root/jobs/fx/cx/v21/s033/lfx-dotnet/raw/hold-L1-L2.window>), rc=0, 9분 29초. 모든 run의 scheduling/affinity와 counts는 각 결과 옆 `scheduling.txt`에서 확인. 상세 절차/HEAD/artifact 기록: [progress.md](/root/jobs/fx/cx/v21/s033/lfx-dotnet/progress.md).
