# C++ WS DEALER/DEALER 결과

release Core `0.10.1`을 사용했고 C와 C++을 직렬로 실행했다. 조건은 `ws`,
`MULTI_DEALER_DEALER`, message size `64,256,1024,4096,65536,131072`, duration 5초,
client 100, auto-HWM `balanced`, connect-ready timeout 10000ms, monitor HWM
4096000이다.

| Size | C throughput (msg/s) | C++ throughput (msg/s) | C 대비 |
|---:|---:|---:|---:|
| 64 | 2,711,546.0 | 1,812,936.0 | 66.86% |
| 256 | 1,436,977.0 | 874,425.6 | 60.85% |
| 1,024 | 920,688.0 | 614,278.0 | 66.72% |
| 4,096 | 451,827.0 | 304,932.0 | 67.49% |
| 65,536 | 64,101.4 | 40,908.0 | 63.82% |
| 131,072 | 33,490.0 | 23,879.4 | 71.30% |
| 산술평균 | - | - | **66.17%** |

목표 90%에 미달한다. 이 transport의 C++ wrapper와 Core 호출 경계를 다음 개선 대상으로
검토한다.

- C: `/tmp/zlink-cpp-dd-ws-c/multi/report/perf_c_multi_linux_20260813_021542_cpp-dd-ws-c.txt`
- C++: `/tmp/zlink-cpp-dd-ws-cpp-final/multi/report/perf_cpp_multi_linux_20260813_022326_cpp-dd-ws-cpp-final.txt`
