# .NET STREAM TCP 결과

| Size | C throughput | .NET throughput | .NET / C |
|---:|---:|---:|---:|
| 64B | 308,863.0 | 254,816.5 | 82.50% |
| 256B | 308,161.5 | 221,373.5 | 71.84% |
| 1024B | 251,770.0 | 219,258.0 | 87.08% |
| 65536B | 45,984.0 | 40,462.0 | 87.99% |
| 산술평균 | - | - | 82.35% |

- Core: release `0.10.1`
- 대상: `MULTI_STREAM / tcp`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: C 종료 후 .NET 실행, 병렬 실행 없음
- C report: `/tmp/zlink-dotnet-stream-c/multi/report/perf_c_multi_linux_20260813_215457_dotnet-stream-c.txt`
- .NET report: `bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260813_215510.txt`
