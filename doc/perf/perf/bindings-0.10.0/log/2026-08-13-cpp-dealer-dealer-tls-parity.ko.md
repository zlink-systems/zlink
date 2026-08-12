# C++ TLS DEALER/DEALER parity 결과

release Core `0.10.1`을 사용했고 C와 C++을 직렬로 실행했다. 조건은 `tls`,
`MULTI_DEALER_DEALER`, message size `64,256,1024,4096,65536,131072`, duration 5초,
client 100, auto-HWM `balanced`, connect-ready timeout 10000ms, monitor HWM
4096000이다.

| Size | C throughput (msg/s) | C++ throughput (msg/s) | C 대비 |
|---:|---:|---:|---:|
| 64 | 2,738,864.2 | 2,366,124.4 | 86.39% |
| 256 | 1,366,366.4 | 1,212,372.8 | 88.73% |
| 1,024 | 883,538.2 | 855,370.0 | 96.81% |
| 4,096 | 384,354.0 | 326,438.2 | 84.93% |
| 65,536 | 41,150.0 | 33,310.4 | 80.95% |
| 131,072 | 16,688.2 | 16,288.4 | 97.60% |
| 산술평균 | - | - | **89.24%** |

C++ runner의 기본 connect-ready timeout과 monitor HWM을 C 기준으로 맞춘 뒤 측정했다.
목표 90%에는 0.76%p 미달한다.

- C: `/tmp/zlink-cpp-dd-tls-c/multi/report/perf_c_multi_linux_20260813_020723_cpp-dd-tls-c.txt`
- C++: `/tmp/zlink-cpp-dd-tls-cpp-parity/multi/report/perf_cpp_multi_linux_20260813_021004_cpp-dd-tls-cpp-parity.txt`
