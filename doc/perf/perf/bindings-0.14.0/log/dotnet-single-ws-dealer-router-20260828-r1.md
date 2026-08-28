# .NET paired measurement: Single / ws / DEALER_ROUTER

- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_091200_dotnet0140-single-ws-dealer_router-r1.txt`
- .NET report: `bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260828_091232_dotnet0140-single-ws-dealer_router-r1.txt`
- status: 미달(84.6%); aggregate throughput=84.64% (target 80%); aggregate mean latency=3.004x (max 3.0x)
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports; runs=1. Terrain-reading single run, not final five-run median.

|Size|64|256|1024|65536|131072|262144|
|---:|---:|---:|---:|---:|---:|---:|
|Throughput ratio|36.86%|34.55%|56.59%|88.82%|151.06%|139.97%|
|Latency ratio|3.184x|2.326x|2.700x|1.000x|7.807x|1.006x|

원시 반복값은 runs=1이므로 각 report의 RESULT 행이며, 위 비율은 그 원시값으로 계산했다. 개선 후보는 구현하지 않았다.
