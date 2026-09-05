# .NET Multi `tcp` 4 pattern before 측정 — Core 0.17.0 (paired C)

- 2026-09-05 11:07~11:14 KST, pattern마다 C 직후 .NET(§7.3), 100 clients, 5초, 1 run, size 64/256/1024/4096/65536, Core `0c39ed2e52` Release+LTO lib(10:48), 순간 load 0.8~5(직전 LTO 빌드의 5/15분 평균은 높음).
- C tag `p1dotnet`: `perf_c_multi_linux_20260905_11{0735,0836,0955,1113}_p1dotnet.txt`; .NET `perf_dotnet_multi_linux_20260905_11{0803,0902,1021,1140}.txt`(`bindings/dotnet/perf/results/multi/report/`).
- 목표(§2.1 .NET): 단순 one-way 64%/85%, request/reply 50%/70%; latency 3.0x.

| pattern | 64 | 256 | 1024 | 4096 | 65536 | aggregate | latency | 상태 |
|---|---|---|---|---|---|---|---|---|
| `MULTI_DEALER_DEALER` | 24.0% (269.7/1121.9) | 27.4% (289.1/1056.1) | 31.1% (295.8/951.9) | 51.0% (192.1/376.5) | 90.0% (71.8/79.8) | **44.7%** | 18.2x | `미달` |
| `MULTI_DEALER_ROUTER_REQREP` | 27.4% (48.9/178.4) | 37.4% (60.7/162.5) | 37.1% (59.6/160.8) | 44.8% (59.9/133.6) | 111.1% (26.6/23.9) | **51.6%** | 0.54x | `미달` |
| `MULTI_ROUTER_ROUTER_REQREP` | 23.5% (42.1/178.8) | 33.1% (53.1/160.5) | 36.9% (53.8/145.8) | 47.6% (53.5/112.6) | 126.9% (24.9/19.6) | **53.6%** | 0.63x | `미달` |
| `MULTI_PUBSUB` | 25.5% (189.2/741.6) | 37.5% (242.3/646.0) | 36.8% (269.1/731.9) | 44.6% (248.7/558.3) | 79.6% (51.0/64.1) | **44.8%** | 1.41x | `미달` |

(괄호 .NET/C Kmsg/s 또는 Kops/s.) 작은 메시지 고정 비용이 C의 3~4배로, 계획서 §2.1의 .NET 과거 p10(단순 one-way 74.9%)보다 크게 낮다 → 0.17.0 wait-token 포트 뒤 .NET 회귀 가능성. DD latency 18.2x는 one-way 큐 깊이(D-B91: 판정 제외)이나 C 대비 처리량이 낮은 상태의 큐 포화라 참고. 자체 pass 1 job(astra, 브리프 `doc/plan/c016-worklog/briefs/dotnet-perf-pass1.prompt`) 11:17 KST 시작.
