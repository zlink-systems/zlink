# .NET paired measurement: Single / tcp / DEALER_ROUTER

- timestamp: 2026-08-28T08:49:53+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_085216_dotnet0140-single-tcp-dealer_router-r1.txt`
- .NET report: `bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260828_085248_dotnet0140-single-tcp-dealer_router-r1.txt`
- status: 미달(64.1%)
- aggregate throughput ratio: 64.11% (target 80%)
- aggregate mean-latency ratio: 29.179x (max 3.0x)
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports
- runs=1. This is a terrain-reading single run, not a final five-run median; run drift may be present.

| Size | C throughput | .NET throughput | Ratio | C mean latency | .NET mean latency | Latency ratio | C raw (throughput/latency) | .NET raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 855246.000 | 280962.600 | 32.85% | 26.132 | 298.062 | 11.406x | 855246.000/26.132 | 280962.600/298.062 |
| 256 | 814366.200 | 276376.200 | 33.94% | 0.263 | 42.104 | 160.091x | 814366.200/0.263 | 276376.200/42.104 |
| 1024 | 829459.600 | 303044.600 | 36.54% | 2.023 | 1.034 | 0.511x | 829459.600/2.023 | 303044.600/1.034 |
| 65536 | 37056.800 | 31004.600 | 83.67% | 0.240 | 0.273 | 1.138x | 37056.800/0.240 | 31004.600/0.273 |
| 131072 | 26814.800 | 30033.600 | 112.00% | 0.205 | 0.193 | 0.941x | 26814.800/0.205 | 30033.600/0.193 |
| 262144 | 16157.400 | 13836.400 | 85.64% | 0.221 | 0.218 | 0.986x | 16157.400/0.221 | 13836.400/0.218 |

## 판정 근거
- throughput aggregate 64.11%와 latency aggregate 29.179x를 사용했다.
- 이번 run은 측정 전용이며, 개선 후보는 구현하지 않았다.
- 후보 기록: 작은 메시지의 managed public send/receive 및 routed dispatch 비용, PUBSUB 대형 셀의 C 대비 상반된 처리량·latency 형태를 다음 단계에서 조사한다.
