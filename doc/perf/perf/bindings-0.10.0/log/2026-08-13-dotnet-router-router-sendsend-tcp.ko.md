# .NET ROUTER/ROUTER send/send TCP 결과

| Size | C throughput | .NET throughput | .NET / C |
|---:|---:|---:|---:|
| 64B | 178,360.5 | 139,063.5 | 77.97% |
| 256B | 172,640.0 | 141,519.0 | 81.97% |
| 1024B | 140,397.0 | 129,987.0 | 92.59% |
| 4096B | 127,243.5 | 124,251.5 | 97.65% |
| 65536B | 28,653.5 | 38,552.5 | 134.55% |
| 131072B | 17,064.0 | 24,217.0 | 141.92% |
| 산술평균 | - | - | 104.44% |

- Core: release `0.10.1`
- 대상: `MULTI_ROUTER_ROUTER_SENDSEND / tcp`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: C 종료 후 .NET 실행, 병렬 실행 없음
- C report: `/tmp/zlink-dotnet-rr-ss-c/multi/report/perf_c_multi_linux_20260813_215144_dotnet-rr-ss-c.txt`
- .NET report: `bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260813_215203.txt`
