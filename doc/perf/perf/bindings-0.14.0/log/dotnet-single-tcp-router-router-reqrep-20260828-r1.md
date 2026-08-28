# .NET paired measurement: Single / tcp / ROUTER_ROUTER_REQREP

- timestamp: 2026-08-28T08:49:53+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_085532_dotnet0140-single-tcp-router_router_reqrep-r1.txt`
- .NET report: `bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260828_085603_dotnet0140-single-tcp-router_router_reqrep-r1.txt`
- status: 미달(51.9%)
- aggregate throughput ratio: 51.85% (target 70%)
- aggregate mean-latency ratio: 1.712x (max 3.0x)
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports
- runs=1. This is a terrain-reading single run, not a final five-run median; run drift may be present.

| Size | C throughput | .NET throughput | Ratio | C mean latency | .NET mean latency | Latency ratio | C raw (throughput/latency) | .NET raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 180817.000 | 53150.600 | 29.39% | 0.240 | 0.514 | 2.142x | 180817.000/0.240 | 53150.600/0.514 |
| 256 | 170177.600 | 54960.800 | 32.30% | 0.215 | 0.446 | 2.074x | 170177.600/0.215 | 54960.800/0.446 |
| 1024 | 157266.600 | 50967.400 | 32.41% | 0.233 | 0.480 | 2.060x | 157266.600/0.233 | 50967.400/0.480 |
| 65536 | 18356.000 | 15111.200 | 82.32% | 0.649 | 0.702 | 1.082x | 18356.000/0.649 | 15111.200/0.702 |
| 131072 | 12441.800 | 8321.400 | 66.88% | 0.479 | 0.695 | 1.451x | 12441.800/0.479 | 8321.400/0.695 |
| 262144 | 7702.800 | 5222.200 | 67.80% | 0.386 | 0.564 | 1.461x | 7702.800/0.386 | 5222.200/0.564 |

## 판정 근거
- throughput aggregate 51.85%와 latency aggregate 1.712x를 사용했다.
- 이번 run은 측정 전용이며, 개선 후보는 구현하지 않았다.
- 후보 기록: 작은 메시지의 managed public send/receive 및 routed dispatch 비용, PUBSUB 대형 셀의 C 대비 상반된 처리량·latency 형태를 다음 단계에서 조사한다.
