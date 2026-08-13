# C++ multi ROUTER/ROUTER request/reply tcp 측정

release Core `0.10.1`을 사용해 C 다음 C++을 각각 한 번 실행했다. 조건은 tcp, client 100,
duration 1초, auto-HWM, message size 64/256/1024/4096/65536/131072 byte다.

| 구현 | 결과 파일 |
|---|---|
| C | `/tmp/zlink-cpp-rrreq-current-c/multi/report/perf_c_multi_linux_20260812_231415.txt` |
| C++ | `/tmp/zlink-cpp-rrreq-current-cpp/multi/report/perf_cpp_multi_linux_20260812_231433.txt` |

| Size | C throughput | C++ throughput | C++/C |
|---:|---:|---:|---:|
| 64B | 103982 | 98655 | 94.88% |
| 256B | 106329 | 94591 | 88.96% |
| 1024B | 92411 | 91268 | 98.76% |
| 4096B | 90223 | 86024 | 95.35% |
| 65536B | 24734 | 24710 | 99.90% |
| 131072B | 15929 | 16007 | 100.49% |
| 산술평균 | - | - | 96.39% |

산술평균은 C++ socket request/reply 목표 85%를 통과했다.
