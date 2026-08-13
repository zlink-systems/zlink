# C++ multi ROUTER/ROUTER SENDSEND tcp 측정

release Core `0.10.1`을 사용해 C 다음 C++을 각각 한 번 실행했다. 조건은 tcp, client 100,
duration 1초, auto-HWM, message size 64/256/1024/4096/65536/131072 byte다.

| 구현 | 결과 파일 |
|---|---|
| C | `/tmp/zlink-cpp-rrss-current-c/multi/report/perf_c_multi_linux_20260812_231235.txt` |
| C++ | `/tmp/zlink-cpp-rrss-current-cpp/multi/report/perf_cpp_multi_linux_20260812_231252.txt` |

| Size | C throughput | C++ throughput | C++/C |
|---:|---:|---:|---:|
| 64B | 191755 | 194114 | 101.23% |
| 256B | 192460 | 189970 | 98.71% |
| 1024B | 185639 | 187780 | 101.15% |
| 4096B | 165676 | 173748 | 104.87% |
| 65536B | 38852 | 38304 | 98.59% |
| 131072B | 22507 | 22317 | 99.16% |
| 산술평균 | - | - | 100.62% |

산술평균은 C++ routed one-way 목표 85%를 통과했다.
