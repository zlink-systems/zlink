# .NET paired measurement: Single / ws / ROUTER_ROUTER_REQREP

- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_091517_dotnet0140-single-ws-router_router_reqrep-r1.txt`
- .NET report: `bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260828_091548_dotnet0140-single-ws-router_router_reqrep-r1.txt`
- status: 미달(74.4%); aggregate throughput=74.43% (target 70%); aggregate mean latency=0.994x (max 3.0x)
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports; runs=1. Terrain-reading single run, not final five-run median.

|Size|64|256|1024|65536|131072|262144|
|---:|---:|---:|---:|---:|---:|---:|
|Throughput ratio|36.04%|53.47%|82.22%|103.23%|96.89%|74.73%|
|Latency ratio|1.478x|0.884x|0.532x|0.910x|0.847x|1.316x|

원시 반복값은 runs=1이므로 각 report의 RESULT 행이며, 위 비율은 그 원시값으로 계산했다. 개선 후보는 구현하지 않았다.
