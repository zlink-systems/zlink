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

## single-thread metrics 후보 결과

- transport: `tcp`
- pattern: `MULTI_DEALER_DEALER`
- 공통 조건: message size `64,256,1024,4096,65536,131072`, client `100`,
  duration `5s`, auto-HWM, I/O thread `4`, release Core `0.10.1`

| Message size | C throughput (msg/s) | Java throughput (msg/s) | C 대비 |
|---:|---:|---:|---:|
| 64 | 2,737,870.2 | 1,662,077.6 | 60.71% |
| 256 | 1,483,876.8 | 1,197,203.8 | 80.68% |
| 1,024 | 1,009,432.0 | 829,059.8 | 82.13% |
| 4,096 | 470,711.8 | 324,799.8 | 69.00% |
| 65,536 | 113,395.8 | 82,098.2 | 72.40% |
| 131,072 | 48,729.4 | 40,514.4 | 83.14% |
| 산술평균 | - | - | **74.68%** |

목표 90%에 미달해 후보 구현은 제거했다.

원본 결과 파일:

- C: `/tmp/zlink-java-single-metrics-c/multi/report/perf_c_multi_linux_20260813_030051_single-metrics-c.txt`
- Java: `/tmp/zlink-java-single-metrics-java/multi/report/perf_java_multi_linux_20260813_030128_single-metrics-java.txt`

## metric header 단일 검증 결과

- transport: `tcp`
- pattern: `MULTI_DEALER_DEALER`
- 공통 조건: message size `64,256,1024,4096,65536,131072`, client `100`,
  duration `5s`, auto-HWM, I/O thread `4`, release Core `0.10.1`

| Message size | C throughput (msg/s) | Java throughput (msg/s) | C 대비 |
|---:|---:|---:|---:|
| 64 | 2,566,250.2 | 1,787,442.4 | 69.65% |
| 256 | 1,406,066.2 | 1,074,557.0 | 76.42% |
| 1,024 | 934,856.0 | 770,997.2 | 82.47% |
| 4,096 | 448,140.8 | 324,259.8 | 72.36% |
| 65,536 | 102,742.8 | 81,374.0 | 79.20% |
| 131,072 | 41,782.2 | 51,262.2 | 122.69% |
| 산술평균 | - | - | **83.80%** |

목표 90%에는 미달한다. message size와 header 범위 검사를 한 번만 수행해 C의
직접 header decode와 같은 측정 의미로 정렬했다.

원본 결과 파일:

- C: `/tmp/zlink-java-header-read-c/multi/report/perf_c_multi_linux_20260813_030501_header-read-c.txt`
- Java: `/tmp/zlink-java-header-read-java/multi/report/perf_java_multi_linux_20260813_030538_header-read-java.txt`

## PUB/SUB header 단일 검증 결과

`MULTI_PUBSUB/tcp`에서 같은 header 단일 검증 경로는 74.63%였다. 목표 90%에는
미달한다.

- C: `/tmp/zlink-java-header-single-call-c/multi/report/perf_c_multi_linux_20260813_031221_header-single-call-c.txt`
- Java: `/tmp/zlink-java-header-single-call-java/multi/report/perf_java_multi_linux_20260813_031258_header-single-call-java.txt`

## receive metadata bridge 후보 결과

size와 data address를 하나의 Java private bridge downcall로 읽는 후보의
`MULTI_PUBSUB/tcp` 평균은 73.49%였다. 기존 74.63%보다 낮아 후보 구현은 제거했다.

- C: `/tmp/zlink-java-metadata-c/multi/report/perf_c_multi_linux_20260813_031622_metadata-c.txt`
- Java: `/tmp/zlink-java-metadata-java/multi/report/perf_java_multi_linux_20260813_031659_metadata-java.txt`
