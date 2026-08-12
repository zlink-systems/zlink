# Java ready-event gate 재측정 결과

## 대상

- transport: `tcp`
- pattern: `MULTI_DEALER_ROUTER_SENDSEND`
- 비교: C와 Java를 순차 실행
- 공통 조건: message size `64,256,1024,4096,65536,131072`, client `100`,
  duration `5s`, auto-HWM, I/O thread `4`, release Core `0.10.1`

## 결과

| Message size | C throughput (ops/s) | Java throughput (ops/s) | C 대비 |
|---:|---:|---:|---:|
| 64 | 191,067.0 | 152,611.6 | 79.87% |
| 256 | 178,150.6 | 150,440.8 | 84.45% |
| 1,024 | 169,977.2 | 143,321.0 | 84.32% |
| 4,096 | 157,553.8 | 118,100.2 | 74.96% |
| 65,536 | 36,695.4 | 42,316.0 | 115.32% |
| 131,072 | 20,509.8 | 23,615.8 | 115.14% |
| 산술평균 | - | - | **92.34%** |

목표 85%를 통과했다.

원본 결과 파일:

- C: `/tmp/zlink-java-ready-gate-c/multi/report/perf_c_multi_linux_20260813_024322_ready-gate-c.txt`
- Java: `/tmp/zlink-java-ready-gate-java/multi/report/perf_java_multi_linux_20260813_024409_ready-gate-java.txt`

## socket request/reply 결과

- transport: `tcp`
- pattern: `MULTI_DEALER_ROUTER_REQREP`
- 공통 조건: message size `64,256,1024,4096,65536,131072`, client `100`,
  duration `5s`, auto-HWM, I/O thread `4`, release Core `0.10.1`

| Message size | C throughput (ops/s) | Java throughput (ops/s) | C 대비 |
|---:|---:|---:|---:|
| 64 | 107,775.4 | 61,742.8 | 57.29% |
| 256 | 98,420.8 | 59,916.4 | 60.88% |
| 1,024 | 92,491.4 | 60,112.6 | 64.99% |
| 4,096 | 86,482.8 | 48,290.4 | 55.84% |
| 65,536 | 26,379.2 | 25,700.6 | 97.43% |
| 131,072 | 15,425.8 | 14,344.8 | 92.99% |
| 산술평균 | - | - | **71.57%** |

목표 70%를 통과했다.

원본 결과 파일:

- C: `/tmp/zlink-java-ready-gate-reqrep-c/multi/report/perf_c_multi_linux_20260813_024624_ready-gate-reqrep-c.txt`
- Java: `/tmp/zlink-java-ready-gate-reqrep-java/multi/report/perf_java_multi_linux_20260813_024703_ready-gate-reqrep-java.txt`

## `Received` two-slot 후보 결과

- transport: `tcp`
- pattern: `MULTI_DEALER_DEALER`
- 공통 조건: message size `64,256,1024,4096,65536,131072`, client `100`,
  duration `5s`, auto-HWM, I/O thread `4`, release Core `0.10.1`

| Message size | C throughput (msg/s) | Java throughput (msg/s) | C 대비 |
|---:|---:|---:|---:|
| 64 | 2,593,897.2 | 1,723,915.4 | 66.46% |
| 256 | 1,394,044.0 | 1,115,337.6 | 80.01% |
| 1,024 | 943,313.4 | 781,752.4 | 82.87% |
| 4,096 | 524,260.8 | 341,592.6 | 65.16% |
| 65,536 | 102,494.6 | 82,871.6 | 80.85% |
| 131,072 | 45,454.2 | 44,914.8 | 98.81% |
| 산술평균 | - | - | **79.03%** |

목표 90%에 미달해 후보 구현은 제거했다.

원본 결과 파일:

- C: `/tmp/zlink-java-received-two-slot-c/multi/report/perf_c_multi_linux_20260813_025441_received-two-slot-c.txt`
- Java: `/tmp/zlink-java-received-two-slot-java/multi/report/perf_java_multi_linux_20260813_025518_received-two-slot-java.txt`
