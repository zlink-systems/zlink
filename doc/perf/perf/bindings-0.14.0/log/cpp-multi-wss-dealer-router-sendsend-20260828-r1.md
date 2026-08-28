# C++ paired measurement: Multi / wss / MULTI_DEALER_ROUTER_SENDSEND

- timestamp: 2026-08-28T08:16:48+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_081648_cpp0140-multi-wss-dealer-router-sendsend-r1.txt`
- status: 미측정
- `runs=1`; this terrain-reading run is not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,4096,65536,131072; duration=5s; part-count=2.
- runtime SHA: Core and C++ native runtime `a6f7a7fb727b7e1e05cc9a7f088376af5a5c34e0fcbc34bc2601b9674b077777` matched before measurement.
- META in the C report: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime=`/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`.

## 측정 불가 근거

- C report가 `status: partial`(success=1, fail=5)로 완료되지 않았다. 64B 원시값은 throughput=113810.000, mean latency=0.410ms이나 partial report는 판정에 사용하지 않았다.
- 256B부터 client가 exit -6으로 종료했고 report는 `malloc_consolidate(): unaligned fastbin chunk detected`를 기록했다. 따라서 같은 manifest의 C++ 비교는 실행하지 않았다.
- 측정 전용 run이므로 Core 또는 binding source를 수정하지 않았다. 후보: C multi `wss` send/send echo의 heap corruption 원인을 별도 Core 조사에서 재현·분리한다.
