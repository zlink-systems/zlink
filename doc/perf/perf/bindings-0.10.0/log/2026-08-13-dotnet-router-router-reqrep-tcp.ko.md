# .NET ROUTER/ROUTER request/reply TCP 결과

| Size | C throughput | .NET throughput | .NET / C |
|---:|---:|---:|---:|
| 64B | 82,681.5 | 58,430.0 | 70.67% |
| 256B | 81,664.5 | 54,861.0 | 67.18% |
| 1024B | 69,419.0 | 53,885.5 | 77.63% |
| 4096B | 66,466.5 | 54,496.5 | 81.99% |
| 65536B | 18,289.5 | 20,959.0 | 114.60% |
| 131072B | 5,312.0 | 16,892.5 | 318.01% |
| 산술평균 | - | - | 121.68% |

- Core: release `0.10.1`
- 대상: `MULTI_ROUTER_ROUTER_REQREP / tcp`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: C 종료 후 .NET 실행, 병렬 실행 없음
- C report: `/tmp/zlink-dotnet-rr-rr-c/multi/report/perf_c_multi_linux_20260813_215256_dotnet-rr-rr-c.txt`
- .NET report: `bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260813_215315.txt`
