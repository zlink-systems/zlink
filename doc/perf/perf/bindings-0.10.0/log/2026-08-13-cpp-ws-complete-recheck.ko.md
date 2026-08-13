# C++ WS complete 재측정

release Core `0.10.1`에서 C 후 C++을 순차 실행했다. 공통 조건은
`MULTI_DEALER_DEALER`, ws, clients `100`, duration `2초`, runs `1`, I/O threads
`4/4`, balanced auto-HWM, send/receive timeout `200ms`다.

- C report: `/tmp/zlink-cpp-ws-recheck-c/multi/report/perf_c_multi_linux_20260813_044934_cpp-ws-recheck-c.txt`
- C++ report: `/tmp/zlink-cpp-ws-recheck2-cpp/multi/report/perf_cpp_multi_linux_20260813_045322_cpp-ws-recheck2-cpp.txt`

| Size | C throughput | C++ throughput | C++ / C |
|---:|---:|---:|---:|
| 64B | 2,405,729 msg/s | 2,543,272 msg/s | 105.72% |
| 256B | 1,380,167 msg/s | 1,368,204 msg/s | 99.13% |
| 1024B | 884,581 msg/s | 907,707 msg/s | 102.61% |
| 4096B | 452,415 msg/s | 420,046 msg/s | 92.84% |
| 65536B | 57,500 msg/s | 57,143 msg/s | 99.38% |
| 131072B | 31,393 msg/s | 32,696 msg/s | 104.15% |

산술평균은 `100.64%`로 목표 `90%`를 통과한다. 두 report는 `status: complete`,
result line `30/30`이다.
