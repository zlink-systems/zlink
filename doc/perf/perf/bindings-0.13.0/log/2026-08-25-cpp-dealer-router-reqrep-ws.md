# C++ Single DEALER_ROUTER_REQREP / ws — Core 0.13.0

## Paired baseline

- branch/source commit: `core-0.13.0-bindings-performance` / `3e7ee7bedb`; source `0.13.2`
- runtime: `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0`, `core/v0.13.0` / `dc9930877041649fc7400de0ebe5382ad9b33ff9`
- smoke C/C++: `perf_c_single_linux_20260825_130148_cpp-dealer-router-reqrep-ws-core0130-smoke-c-20260825.txt`; `perf_cpp_single_linux_20260825_130149_cpp-dealer-router-reqrep-ws-core0130-smoke-cpp-20260825.txt`
- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_130158_cpp-dealer-router-reqrep-ws-core0130-before-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_130336_cpp-dealer-router-reqrep-ws-core0130-before-cpp-20260825.txt`
- two paired reports: `status: complete`, 30/30 result lines; runtime/revision/tag, Effective Options, auto-HWM, six sizes match.

| Size | throughput ratio | latency ratio |
|------|-----------------:|--------------:|
| 64 | 31.84% | 3.622x |
| 256 | 31.44% | 3.650x |
| 1024 | 30.66% | 3.438x |
| 65536 | 84.13% | 1.175x |
| 131072 | 76.84% | 1.280x |
| 262144 | 97.27% | 0.996x |

- throughput ratio 산술평균: **58.70%** — C++ socket request/reply 목표 85% 미달
- mean-latency ratio 산술평균: **2.360x** — 일반 2.0x 상한도 초과

WebSocket setup은 활성 구간 밖이다. 활성 request/reply는 public `request` future 결과의
completion 관찰과 Router public reply terminal로 구성되며, routing/ownership, timeout,
in-flight 제한, completion/close failure의 의미를 보존해야 한다. C API/private path,
native future/message 재사용, completion synchronization 제거, ownership 변경이나 pool 증설은
no-go다. 안전한 binding 변경 후보 없이 throughput과 latency가 모두 미달하므로 source 변경 없이
**DEALER_ROUTER_REQREP/ws는 보류**다. 목표 경계가 아니므로 final 5회는 요구되지 않는다.
