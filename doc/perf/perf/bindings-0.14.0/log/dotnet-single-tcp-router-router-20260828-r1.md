# .NET paired measurement: Single / tcp / ROUTER_ROUTER

- timestamp: 2026-08-28T08:49:53+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_085426_dotnet0140-single-tcp-router_router-r1.txt`
- .NET report: `bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260828_085458_dotnet0140-single-tcp-router_router-r1.txt`
- status: 미달(59.5%)
- aggregate throughput ratio: 59.49% (target 80%)
- aggregate mean-latency ratio: 38.548x (max 3.0x)
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports
- runs=1. This is a terrain-reading single run, not a final five-run median; run drift may be present.

| Size | C throughput | .NET throughput | Ratio | C mean latency | .NET mean latency | Latency ratio | C raw (throughput/latency) | .NET raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 848848.600 | 333419.000 | 39.28% | 0.210 | 36.230 | 172.524x | 848848.600/0.210 | 333419.000/36.230 |
| 256 | 773643.600 | 314271.200 | 40.62% | 0.189 | 10.305 | 54.524x | 773643.600/0.189 | 314271.200/10.305 |
| 1024 | 739746.200 | 293596.200 | 39.69% | 8.710 | 0.234 | 0.027x | 739746.200/8.710 | 293596.200/0.234 |
| 65536 | 37009.800 | 28300.000 | 76.47% | 0.242 | 0.309 | 1.277x | 37009.800/0.242 | 28300.000/0.309 |
| 131072 | 26085.600 | 22243.000 | 85.27% | 0.210 | 0.285 | 1.357x | 26085.600/0.210 | 22243.000/0.285 |
| 262144 | 16282.000 | 12306.800 | 75.59% | 0.196 | 0.310 | 1.582x | 16282.000/0.196 | 12306.800/0.310 |

## 판정 근거
- throughput aggregate 59.49%와 latency aggregate 38.548x를 사용했다.
- 이번 run은 측정 전용이며, 개선 후보는 구현하지 않았다.
- 후보 기록: 작은 메시지의 managed public send/receive 및 routed dispatch 비용, PUBSUB 대형 셀의 C 대비 상반된 처리량·latency 형태를 다음 단계에서 조사한다.
