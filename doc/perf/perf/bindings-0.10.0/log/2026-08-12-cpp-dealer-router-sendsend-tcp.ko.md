# C++ multi DEALER/ROUTER SENDSEND tcp 측정

release Core `0.10.1`을 사용해 C 다음 C++을 각각 한 번 실행했다. 조건은 tcp, client 100,
duration 1초, auto-HWM, message size 64/256/1024/4096/65536/131072 byte다.

| 구현 | 결과 파일 |
|---|---|
| C | `/tmp/zlink-cpp-drss-current-c/multi/report/perf_c_multi_linux_20260812_230925.txt` |
| C++ | `/tmp/zlink-cpp-drss-current-cpp/multi/report/perf_cpp_multi_linux_20260812_230940.txt` |

| Size | C throughput | C++ throughput | C++/C |
|---:|---:|---:|---:|
| 64B | 178109 | 186375 | 104.64% |
| 256B | 181545 | 181168 | 99.79% |
| 1024B | 184161 | 182317 | 99.00% |
| 4096B | 166150 | 164924 | 99.26% |
| 65536B | 37364 | 34914 | 93.44% |
| 131072B | 20981 | 21484 | 102.40% |
| 산술평균 | - | - | 99.75% |

산술평균은 C++ routed one-way 목표 85%를 통과했다.
