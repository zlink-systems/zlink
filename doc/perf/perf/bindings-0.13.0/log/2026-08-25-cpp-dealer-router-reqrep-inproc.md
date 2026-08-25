# C++ Single DEALER_ROUTER_REQREP / inproc — Core 0.13.0

- commit/source: `3b9d259012` / `0.13.2`; Core `0.13.0` (`core/v0.13.0`, `dc9930877041649fc7400de0ebe5382ad9b33ff9`)
- smoke C/C++: `perf_c_single_linux_20260825_131800_cpp-dealer-router-reqrep-inproc-core0130-smoke-c-20260825.txt`; `perf_cpp_single_linux_20260825_131802_cpp-dealer-router-reqrep-inproc-core0130-smoke-cpp-20260825.txt`
- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_131809_cpp-dealer-router-reqrep-inproc-core0130-before-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_131947_cpp-dealer-router-reqrep-inproc-core0130-before-cpp-20260825.txt`
- both reports `status: complete`, 30/30 results; runtime/options/HWM/sizes match.

| Size | throughput ratio | latency ratio |
|------|-----------------:|--------------:|
| 64 | 38.98% | 0.983x |
| 256 | 37.47% | 0.984x |
| 1024 | 37.71% | 0.832x |
| 65536 | 26.33% | 1.603x |
| 131072 | 29.38% | 1.891x |
| 262144 | 57.98% | 1.561x |

- throughput aggregate **37.98%**, socket request/reply target 85% 미달
- latency aggregate **1.309x**, 2.0x 상한 통과

The C++ active range maintains public request futures, server reply identity/ownership, in-flight
count-and-byte limit and completion/close semantics. C/private API or native future/message reuse,
completion synchronization removal and pool expansion are no-go. The inproc cliff spans the Core/native
path and no safe binding candidate reaches the target, so **DEALER_ROUTER_REQREP/inproc는 보류**다.
