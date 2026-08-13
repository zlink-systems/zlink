# C++ multi PUB/SUB tcp 측정

release Core `0.10.1`을 사용해 C 다음 C++을 각각 한 번 실행했다. 조건은 tcp, client 100,
duration 1초, auto-HWM, message size 64/256/1024/4096/65536/131072 byte다.

| 구현 | 결과 파일 |
|---|---|
| C | `/tmp/zlink-cpp-pub-current-c/multi/report/perf_c_multi_linux_20260812_231547.txt` |
| C++ | `/tmp/zlink-cpp-pub-current-cpp/multi/report/perf_cpp_multi_linux_20260812_231600.txt` |

| Size | C throughput | C++ throughput | C++/C |
|---:|---:|---:|---:|
| 64B | 1426336 | 1521262 | 106.66% |
| 256B | 1526628 | 1400227 | 91.72% |
| 1024B | 1239302 | 1243491 | 100.34% |
| 4096B | 549820 | 452188 | 82.24% |
| 65536B | 112970 | 106066 | 93.89% |
| 131072B | 56890 | 52227 | 91.80% |
| 산술평균 | - | - | 94.49% |

산술평균은 C++ 단순 one-way 목표 90%를 통과했다.
