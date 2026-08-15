# .NET DEALER/ROUTER send/send TCP 결과

| Size | C throughput | .NET throughput | .NET / C |
|---:|---:|---:|---:|
| 64B | 159,626.0 | 139,117.5 | 87.15% |
| 256B | 143,007.0 | 127,664.0 | 89.27% |
| 1024B | 133,850.5 | 121,588.5 | 90.84% |
| 4096B | 118,864.0 | 117,436.0 | 98.80% |
| 65536B | 27,400.0 | 37,382.5 | 136.43% |
| 131072B | 17,133.5 | 24,798.5 | 144.74% |
| 산술평균 | - | - | 107.87% |

- Core: release `0.10.1`
- 대상: `MULTI_DEALER_ROUTER_SENDSEND / tcp`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: C 종료 후 .NET 실행, 병렬 실행 없음
- C report: `/tmp/zlink-dotnet-dr-ss-c/multi/report/perf_c_multi_linux_20260813_214823_dotnet-dr-ss-c.txt`
- .NET report: `bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260813_214845.txt`
