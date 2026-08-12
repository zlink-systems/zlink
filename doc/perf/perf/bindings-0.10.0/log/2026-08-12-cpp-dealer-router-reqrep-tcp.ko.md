# C++ multi DEALER/ROUTER request/reply tcp 측정

release Core `0.10.1`을 사용해 C 다음 C++을 각각 한 번 실행했다. 조건은 tcp, client 100,
duration 1초, auto-HWM, message size 64/256/1024/4096/65536/131072 byte다.

| 구현 | 결과 파일 |
|---|---|
| C | `/tmp/zlink-cpp-drreq-current-c/multi/report/perf_c_multi_linux_20260812_231133.txt` |
| C++ | `/tmp/zlink-cpp-drreq-current-cpp/multi/report/perf_cpp_multi_linux_20260812_231148.txt` |

| Size | C throughput | C++ throughput | C++/C |
|---:|---:|---:|---:|
| 64B | 109421 | 103003 | 94.14% |
| 256B | 104874 | 100518 | 95.85% |
| 1024B | 98308 | 95243 | 96.88% |
| 4096B | 95603 | 88865 | 92.95% |
| 65536B | 27372 | 24520 | 89.58% |
| 131072B | 16720 | 16027 | 95.86% |
| 산술평균 | - | - | 94.21% |

산술평균은 C++ socket request/reply 목표 85%를 통과했다.
