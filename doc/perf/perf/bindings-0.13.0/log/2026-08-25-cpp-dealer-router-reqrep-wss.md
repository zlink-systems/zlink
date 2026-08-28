# C++ Single DEALER_ROUTER_REQREP / wss — Core 0.13.0

Secure transport final judgment used C→C++ five-run medians.

- commit/source: `89217e00fc` / `0.13.2`; Core `0.13.0`, tag `core/v0.13.0`, revision `dc9930877041649fc7400de0ebe5382ad9b33ff9`
- smoke: `perf_c_single_linux_20260825_130551_cpp-dealer-router-reqrep-wss-core0130-smoke-c-20260825.txt`; `perf_cpp_single_linux_20260825_130552_cpp-dealer-router-reqrep-wss-core0130-smoke-cpp-20260825.txt`
- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_130600_cpp-dealer-router-reqrep-wss-core0130-final5-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_130838_cpp-dealer-router-reqrep-wss-core0130-final5-cpp-20260825.txt`
- Both reports are `status: complete`, 30/30 result lines; runtime/revision/tag, Effective Options, auto-HWM and six sizes match.

| Size | throughput ratio | latency ratio |
|------|-----------------:|--------------:|
| 64 | 40.01% | 2.807x |
| 256 | 36.34% | 2.970x |
| 1024 | 37.74% | 2.692x |
| 65536 | 94.40% | 1.052x |
| 131072 | 89.15% | 1.109x |
| 262144 | 89.56% | 1.095x |

- throughput aggregate: **64.37%**, socket request/reply target 85% 미달
- mean-latency aggregate: **1.954x**, C++ 2.0x 상한 통과

WSS/TLS setup is outside the active range. The active public request future, Router reply terminal,
completion synchronization and count/byte in-flight bound preserve request/reply ownership, timeout,
failure and close semantics. Bypassing them through C/private APIs, native future/message reuse,
synchronization removal or pool expansion is no-go. No safe binding candidate reaches the target, so
**DEALER_ROUTER_REQREP/wss는 보류**다.
