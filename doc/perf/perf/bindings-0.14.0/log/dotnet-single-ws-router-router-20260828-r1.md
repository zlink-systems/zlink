# .NET paired measurement: Single / ws / ROUTER_ROUTER

- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_091411_dotnet0140-single-ws-router_router-r1.txt`
- .NET report: `bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260828_091443_dotnet0140-single-ws-router_router-r1.txt`
- status: 미달(66.7%); aggregate throughput=66.72% (target 80%); aggregate mean latency=0.974x (max 3.0x)
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports; runs=1. Terrain-reading single run, not final five-run median.

|Size|64|256|1024|65536|131072|262144|
|---:|---:|---:|---:|---:|---:|---:|
|Throughput ratio|37.32%|42.23%|44.78%|81.32%|90.24%|104.45%|
|Latency ratio|0.845x|1.016x|0.019x|1.480x|0.933x|1.554x|

원시 반복값은 runs=1이므로 각 report의 RESULT 행이며, 위 비율은 그 원시값으로 계산했다. 개선 후보는 구현하지 않았다.
