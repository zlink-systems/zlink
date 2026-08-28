# C++ Single DEALER_ROUTER_REQREP / tcp — Core 0.13.0

## Manifest and paired baseline

| 항목 | 값 |
|------|----|
| branch / source-report commit | `core-0.13.0-bindings-performance` / `e22d700c39` |
| source / Core runtime | `0.13.2` / `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` |
| Core tag / revision | `core/v0.13.0` / `dc9930877041649fc7400de0ebe5382ad9b33ff9` |

- smoke C/C++: `perf_c_single_linux_20260825_125726_cpp-dealer-router-reqrep-tcp-core0130-smoke-c-20260825.txt`; `perf_cpp_single_linux_20260825_125727_cpp-dealer-router-reqrep-tcp-core0130-smoke-cpp-20260825.txt`
- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_125742_cpp-dealer-router-reqrep-tcp-core0130-before-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_125919_cpp-dealer-router-reqrep-tcp-core0130-before-cpp-20260825.txt`
- 두 paired report `status: complete`, 30/30 result lines; runtime/revision/tag, Effective Options, auto-HWM 및 six sizes 일치.

| Size | throughput ratio | latency ratio |
|------|-----------------:|--------------:|
| 64 | 57.70% | 1.871x |
| 256 | 55.65% | 2.040x |
| 1024 | 34.61% | 3.079x |
| 65536 | 69.12% | 1.429x |
| 131072 | 83.44% | 1.168x |
| 262144 | 83.99% | 1.145x |

- throughput ratio 산술평균: **64.09%** — C++ socket request/reply 목표 85% 미달
- mean-latency ratio 산술평균: **1.789x**

## 공개 경로 검토

활성 요청은 public `socket.request(message_t, timeout)`가 반환하는 `async_result_t`를 completion
관찰자로 연결한다. server의 `received.reply().message(part).submit()`은 Router가 request identity를
보존해 reply를 연결하는 public terminal이다. in-flight count, completion mutex/condition, timeout과
drain은 C 기준의 count-and-byte bound 및 종료 의미를 지킨다. 이들을 없애거나 private/C API,
raw native message/future 경로로 치환하면 request/reply ownership, timeout, failure/close 계약과
측정 정책을 위반한다.

안전한 binding 변경 후보가 없고 목표에 크게 미달하므로 source 변경 없이
**DEALER_ROUTER_REQREP/tcp는 보류**다. 목표 경계가 아니므로 final 5회는 요구되지 않는다.
