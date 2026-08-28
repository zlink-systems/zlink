# C++ Single DEALER_ROUTER_REQREP / tls — Core 0.13.0

Secure final: C→C++ 5-run medians, both reports `status: complete` with 30/30 results and matching runtime/options/HWM/sizes.

- commit/source: `67fde2d25e` / `0.13.2`; Core `0.13.0` `core/v0.13.0` (`dc9930877041649fc7400de0ebe5382ad9b33ff9`)
- smoke C/C++: `perf_c_single_linux_20260825_131154_cpp-dealer-router-reqrep-tls-core0130-smoke-c-20260825.txt`; `perf_cpp_single_linux_20260825_131155_cpp-dealer-router-reqrep-tls-core0130-smoke-cpp-20260825.txt`
- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_131203_cpp-dealer-router-reqrep-tls-core0130-final5-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_131441_cpp-dealer-router-reqrep-tls-core0130-final5-cpp-20260825.txt`

| Size | throughput ratio | latency ratio |
|------|-----------------:|--------------:|
| 64 | 50.12% | 2.337x |
| 256 | 58.15% | 1.957x |
| 1024 | 48.89% | 2.129x |
| 65536 | 92.28% | 1.075x |
| 131072 | 88.09% | 1.117x |
| 262144 | 95.52% | 1.019x |

- throughput aggregate: **72.17%**, socket request/reply target 85% 미달
- mean-latency aggregate: **1.606x**, 2.0x 상한 통과

TLS setup is outside active measurement. The public request future, reply terminal and its ownership,
timeout, in-flight and completion/close semantics cannot be removed or replaced by C/private/native
paths, object reuse or pool growth. No safe binding improvement reaches the target; therefore
**DEALER_ROUTER_REQREP/tls는 보류**다.
