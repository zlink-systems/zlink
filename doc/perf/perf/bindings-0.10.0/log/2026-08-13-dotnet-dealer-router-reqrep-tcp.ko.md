# .NET DEALER/ROUTER request/reply TCP 결과

| Size | C throughput | .NET throughput | .NET / C |
|---:|---:|---:|---:|
| 64B | 93,615.0 | 62,386.5 | 66.64% |
| 256B | 93,362.0 | 57,853.5 | 61.97% |
| 1024B | 82,125.5 | 60,637.5 | 73.83% |
| 4096B | 68,523.5 | 56,736.0 | 82.80% |
| 65536B | 18,645.5 | 24,012.0 | 128.78% |
| 131072B | 13,146.0 | 16,017.5 | 121.84% |
| 산술평균 | - | - | 89.31% |

- Core: release `0.10.1`
- 대상: `MULTI_DEALER_ROUTER_REQREP / tcp`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: C 종료 후 .NET 실행, 병렬 실행 없음
- C report: `/tmp/zlink-dotnet-dr-rr-c/multi/report/perf_c_multi_linux_20260813_214954_dotnet-dr-rr-c.txt`
- .NET report: `bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260813_215015.txt`
