# .NET paired measurement: Single / ws / DEALER_ROUTER_REQREP

- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_091306_dotnet0140-single-ws-dealer_router_reqrep-r1.txt`
- .NET report: `bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260828_091338_dotnet0140-single-ws-dealer_router_reqrep-r1.txt`
- status: 미달(84.5%); aggregate throughput=84.52% (target 70%); aggregate mean latency=1.011x (max 3.0x)
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports; runs=1. Terrain-reading single run, not final five-run median.

|Size|64|256|1024|65536|131072|262144|
|---:|---:|---:|---:|---:|---:|---:|
|Throughput ratio|34.91%|52.38%|79.89%|137.34%|98.42%|104.17%|
|Latency ratio|2.161x|0.818x|0.562x|0.569x|1.001x|0.952x|

원시 반복값은 runs=1이므로 각 report의 RESULT 행이며, 위 비율은 그 원시값으로 계산했다. 개선 후보는 구현하지 않았다.
