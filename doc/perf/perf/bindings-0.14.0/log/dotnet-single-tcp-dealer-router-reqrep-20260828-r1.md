# .NET paired measurement: Single / tcp / DEALER_ROUTER_REQREP

- timestamp: 2026-08-28T08:49:53+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_085322_dotnet0140-single-tcp-dealer_router_reqrep-r1.txt`
- .NET report: `bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260828_085353_dotnet0140-single-tcp-dealer_router_reqrep-r1.txt`
- status: 미달(54.3%)
- aggregate throughput ratio: 54.30% (target 70%)
- aggregate mean-latency ratio: 1.656x (max 3.0x)
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports
- runs=1. This is a terrain-reading single run, not a final five-run median; run drift may be present.

| Size | C throughput | .NET throughput | Ratio | C mean latency | .NET mean latency | Latency ratio | C raw (throughput/latency) | .NET raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 176336.600 | 55183.400 | 31.29% | 0.229 | 0.447 | 1.952x | 176336.600/0.229 | 55183.400/0.447 |
| 256 | 180824.200 | 56342.600 | 31.16% | 0.227 | 0.451 | 1.987x | 180824.200/0.227 | 56342.600/0.451 |
| 1024 | 172252.800 | 55182.000 | 32.04% | 0.215 | 0.473 | 2.200x | 172252.800/0.215 | 55182.000/0.473 |
| 65536 | 18877.200 | 16913.600 | 89.60% | 0.632 | 0.636 | 1.006x | 18877.200/0.632 | 16913.600/0.636 |
| 131072 | 13084.200 | 10182.200 | 77.82% | 0.455 | 0.565 | 1.242x | 13084.200/0.455 | 10182.200/0.565 |
| 262144 | 7900.000 | 5049.200 | 63.91% | 0.376 | 0.582 | 1.548x | 7900.000/0.376 | 5049.200/0.582 |

## 판정 근거
- throughput aggregate 54.30%와 latency aggregate 1.656x를 사용했다.
- 이번 run은 측정 전용이며, 개선 후보는 구현하지 않았다.
- 후보 기록: 작은 메시지의 managed public send/receive 및 routed dispatch 비용, PUBSUB 대형 셀의 C 대비 상반된 처리량·latency 형태를 다음 단계에서 조사한다.
