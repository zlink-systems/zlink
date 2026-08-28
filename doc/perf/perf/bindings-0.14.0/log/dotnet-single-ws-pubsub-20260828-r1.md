# .NET paired measurement: Single / ws / PUBSUB

- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_090937_dotnet0140-single-ws-pubsub-r1.txt`
- .NET report: `bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260828_091015_dotnet0140-single-ws-pubsub-r1.txt`
- status: 미달(222.6%); aggregate throughput=222.60% (target 85%); aggregate mean latency=4.214x (max 3.0x)
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports; runs=1. Terrain-reading single run, not final five-run median.

|Size|64|256|1024|65536|131072|262144|
|---:|---:|---:|---:|---:|---:|---:|
|Throughput ratio|46.20%|45.66%|64.60%|257.86%|396.23%|525.01%|
|Latency ratio|1.421x|2.271x|1.119x|12.678x|6.583x|1.211x|

원시 반복값은 runs=1이므로 각 report의 RESULT 행이며, 위 비율은 그 원시값으로 계산했다. 개선 후보는 구현하지 않았다.
