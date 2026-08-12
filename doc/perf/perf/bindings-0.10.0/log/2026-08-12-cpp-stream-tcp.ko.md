# C++ multi STREAM tcp 측정

release Core `0.10.1`을 사용해 C 다음 C++을 각각 한 번 실행했다. 조건은 tcp, client 100,
duration 1초, auto-HWM, message size 64/256/1024/65536 byte다.

| 구현 | 결과 파일 |
|---|---|
| C | `/tmp/zlink-cpp-stream-current-c/multi/report/perf_c_multi_linux_20260812_231641.txt` |
| C++ | `/tmp/zlink-cpp-stream-current-cpp/multi/report/perf_cpp_multi_linux_20260812_231652.txt` |

| Size | C throughput | C++ throughput | C++/C |
|---:|---:|---:|---:|
| 64B | 340753 | 339045 | 99.50% |
| 256B | 333598 | 333043 | 99.83% |
| 1024B | 328863 | 317688 | 96.60% |
| 65536B | 57947 | 57672 | 99.53% |
| 산술평균 | - | - | 98.85% |

산술평균은 C++ multi routed echo 목표 85%를 통과했다.
