# .NET paired measurement: Single / ws / DEALER_DEALER

- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_091055_dotnet0140-single-ws-dealer_dealer-r1.txt`
- .NET report: `bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260828_091127_dotnet0140-single-ws-dealer_dealer-r1.txt`
- status: 미달(82.8%); aggregate throughput=82.80% (target 85%); aggregate mean latency=2.842x (max 3.0x)
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports; runs=1. Terrain-reading single run, not final five-run median.

|Size|64|256|1024|65536|131072|262144|
|---:|---:|---:|---:|---:|---:|---:|
|Throughput ratio|45.01%|34.64%|56.43%|129.70%|132.66%|98.35%|
|Latency ratio|1.462x|2.301x|1.438x|0.755x|10.161x|0.935x|

원시 반복값은 runs=1이므로 각 report의 RESULT 행이며, 위 비율은 그 원시값으로 계산했다. 개선 후보는 구현하지 않았다.
