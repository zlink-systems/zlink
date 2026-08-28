# .NET paired measurement: Single / tcp / DEALER_DEALER

- timestamp: 2026-08-28T08:49:53+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_085111_dotnet0140-single-tcp-dealer_dealer-r1.txt`
- .NET report: `bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260828_085143_dotnet0140-single-tcp-dealer_dealer-r1.txt`
- status: 미달(61.9%)
- aggregate throughput ratio: 61.94% (target 85%)
- aggregate mean-latency ratio: 3.336x (max 3.0x)
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports
- runs=1. This is a terrain-reading single run, not a final five-run median; run drift may be present.

| Size | C throughput | .NET throughput | Ratio | C mean latency | .NET mean latency | Latency ratio | C raw (throughput/latency) | .NET raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 854915.400 | 302012.600 | 35.33% | 29.052 | 116.451 | 4.008x | 854915.400/29.052 | 302012.600/116.451 |
| 256 | 794706.200 | 307542.600 | 38.70% | 0.662 | 8.513 | 12.860x | 794706.200/0.662 | 307542.600/8.513 |
| 1024 | 809753.600 | 291592.000 | 36.01% | 3.798 | 0.574 | 0.151x | 809753.600/3.798 | 291592.000/0.574 |
| 65536 | 36781.600 | 30438.200 | 82.75% | 0.245 | 0.275 | 1.122x | 36781.600/0.245 | 30438.200/0.275 |
| 131072 | 26534.200 | 23751.800 | 89.51% | 0.205 | 0.209 | 1.020x | 26534.200/0.205 | 23751.800/0.209 |
| 262144 | 16514.000 | 14754.000 | 89.34% | 0.240 | 0.205 | 0.854x | 16514.000/0.240 | 14754.000/0.205 |

## 판정 근거
- throughput aggregate 61.94%와 latency aggregate 3.336x를 사용했다.
- 이번 run은 측정 전용이며, 개선 후보는 구현하지 않았다.
- 후보 기록: 작은 메시지의 managed public send/receive 및 routed dispatch 비용, PUBSUB 대형 셀의 C 대비 상반된 처리량·latency 형태를 다음 단계에서 조사한다.
