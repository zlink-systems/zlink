# .NET paired measurement: Single / tcp / PUBSUB

- timestamp: 2026-08-28T08:49:53+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_084953_dotnet0140-single-tcp-pubsub-r1.txt`
- .NET report: `bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260828_085030_dotnet0140-single-tcp-pubsub-r1.txt`
- status: 미달(284.1%)
- aggregate throughput ratio: 284.12% (target 85%)
- aggregate mean-latency ratio: 164.404x (max 3.0x)
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports
- runs=1. This is a terrain-reading single run, not a final five-run median; run drift may be present.

| Size | C throughput | .NET throughput | Ratio | C mean latency | .NET mean latency | Latency ratio | C raw (throughput/latency) | .NET raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 778187.000 | 338777.800 | 43.53% | 0.129 | 104.483 | 809.946x | 778187.000/0.129 | 338777.800/104.483 |
| 256 | 733823.600 | 313592.600 | 42.73% | 0.153 | 25.409 | 166.072x | 733823.600/0.153 | 313592.600/25.409 |
| 1024 | 704051.600 | 315935.800 | 44.87% | 0.617 | 4.134 | 6.700x | 704051.600/0.617 | 315935.800/4.134 |
| 65536 | 13057.600 | 62949.600 | 482.09% | 0.222 | 0.276 | 1.243x | 13057.600/0.222 | 62949.600/0.276 |
| 131072 | 6073.800 | 32698.200 | 538.35% | 0.207 | 0.243 | 1.174x | 6073.800/0.207 | 32698.200/0.243 |
| 262144 | 2593.800 | 14346.600 | 553.11% | 0.196 | 0.253 | 1.291x | 2593.800/0.196 | 14346.600/0.253 |

## 판정 근거
- throughput aggregate 284.12%와 latency aggregate 164.404x를 사용했다.
- 이번 run은 측정 전용이며, 개선 후보는 구현하지 않았다.
- 후보 기록: 작은 메시지의 managed public send/receive 및 routed dispatch 비용, PUBSUB 대형 셀의 C 대비 상반된 처리량·latency 형태를 다음 단계에서 조사한다.
