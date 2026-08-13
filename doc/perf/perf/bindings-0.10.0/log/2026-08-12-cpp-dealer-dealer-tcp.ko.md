# C++ multi DEALER/DEALER tcp 측정

release Core `0.10.1`을 사용해 C와 C++을 순서대로 각각 한 번 실행했다. 조건은 tcp,
client 100, duration 1초, auto-HWM, message size 64/256/1024/4096/65536/131072 byte다.
C++ binding build는 `ZLINK_CPP_BUILD_BENCHMARKS=ON`으로 구성했고 Core source는 build하지
않았다.

| 구현 | 결과 파일 |
|---|---|
| C | `/tmp/zlink-cpp-dd-current-c/multi/report/perf_c_multi_linux_20260812_223534.txt` |
| C++ | `/tmp/zlink-cpp-dd-current-cpp/multi/report/perf_cpp_multi_linux_20260812_224728.txt` |

| Size | C throughput | C++ throughput | C++/C |
|---:|---:|---:|---:|
| 64B | 2767991 | 2386652 | 86.22% |
| 256B | 1525028 | 1361264 | 89.26% |
| 1024B | 675399 | 984116 | 145.71% |
| 4096B | 323434 | 253731 | 78.45% |
| 65536B | 88447 | 75976 | 85.90% |
| 131072B | 53218 | 42282 | 79.45% |
| 산술평균 | - | - | 94.17% |

이 작업에서 C++ 단순 one-way 완료 목표는 사용자 지시에 따라 90%다. 따라서 이 대상은
통과다.
