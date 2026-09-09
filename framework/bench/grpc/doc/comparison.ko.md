# gRPC 비교 보고서 — framework messaging bench 2차 (2026-09-09)

이 문서는 [벤치 규격](../README.ko.md)의 server-driven 모델(§10)로 잰 2차 측정의 공개 결과다.
언어별 실행 방법과 고유 값은 [.NET](dotnet.ko.md) · [Node.js](node.ko.md) · [Java/Kotlin](java.ko.md) ·
[C++](cpp.ko.md) 문서가, 판정 규칙은 규격 §7이 소유한다. 이 보고서는 집계기 출력만 인용한다.

## 1. 무엇을 비교하는가

규격 §0의 질문은 "같은 언어 안에서 ZLink Framework의 채널 messaging이 gRPC unary 호출과 견줄 만한가,
그리고 Framework 계층이 raw binding 위에 얼마를 얹는가"다. 언어마다 세 구현을 같은 조건으로 잰다.

| 행 | 뜻 |
|---|---|
| `grpc-<lang>` | 그 언어의 표준 gRPC unary `Echo`(request) / `Command`(send) |
| `zlink-<lang>` | ZLink binding의 raw ROUTER request/send |
| `zlink-framework-<lang>` | ZLink Framework의 RouteMesh typed channel request/send |
| `grpc-c` / `zlink-c` | C harness 기준선. ZLink Core가 이 머신에서 낼 수 있는 바닥값 |

측정 모델(규격 §3·§10): source A가 HTTP trigger로 warmup·active를 받고 target B로 메시지를 보낸다.
5초 active 구간의 완료 수(request)와 B가 active header로 받은 수(send)를 세고, 3-run 중앙값을 싣는다.
패턴은 `request-serial`(in-flight 1), `request-window`(in-flight 100), `request-backpressure`(상한 없음),
`send-saturation`(stream 8, 답 없음)이고 payload는 1024·4096 B다. 모든 측정은 같은 WSL 머신의 loopback,
조용한 창(perf 티켓 큐)에서 했다.

| 언어 | 런타임 | gRPC | ZLink binding / Core | Framework |
|---|---|---|---|---|
| C | GCC 13.3 | grpc 1.51.1 (C++) | Core 0.17.5 | — |
| .NET | SDK 8.0.130 / runtime 8.0.30 | Grpc.Net.Client·Grpc.AspNetCore 2.62.0 | 0.17.6 / 0.17.5 | 0.11.0 소스 |
| Java / Kotlin | Temurin 22.0.2, Kotlin 2.2.21 | grpc-java 1.72.0 / grpc-kotlin 1.4.1 | 0.17.6 / 0.17.5 | 0.11.0 소스 |
| Node.js | v22.23.2 | @grpc/grpc-js 1.14.4 | 0.17.6 / 0.17.5 | 0.11.0 소스 (framework 행 unsupported) |
| C++ | GCC 13.3, C++20, `-O3` | grpc++ 1.51.1 | 0.17.6 / 0.17.5 | 0.11.0 소스 |

## 2. 결과 요약

단위: request 계열은 **KOPS**(초당 천 건의 request/reply 완료), `send-saturation`은 **KMSG/s**(B가 active
header로 받은 초당 천 건 메시지). 값은 warmup 뒤 5초 active 구간의 평균이며 3-run의 중앙값이다. 지연은
request 계열은 A의 왕복, send는 B의 header 기준 도착 지연이다(C harness는 send 지연을 재지 않는다).
`drain`은 active가 닫힌 뒤 count가 멈출 때까지의 시간이다(FB-051: drain이 active의 10%를 넘는 send 행은
`수신 수 / (5000 + drain)`이 실제 소비율이다). 비고의 `G5 n%`는 3-run 재현성 상한 10%를 넘긴 행이고,
`FB-nnn`은 제품 결함으로 기록된 행이다. 두 표시가 있는 행은 판정에 쓰지 않는다.

#### request-serial

| 구현 | payload | 처리량 (KOPS) | 지연 mean (ms) | p95 (ms) | p99 (ms) | drain (ms) | 비고 |
|---|---:|---:|---:|---:|---:|---:|---|
| `grpc-c` | 1024 | 13.63 | 0.071 | 0.104 | 0.157 | — |  |
| `grpc-c` | 4096 | 12.90 | 0.074 | 0.110 | 0.156 | — |  |
| `zlink-c` | 1024 | 8.00 | 0.124 | 0.151 | 0.225 | — | G5 10.6% |
| `zlink-c` | 4096 | 8.00 | 0.124 | 0.152 | 0.225 | — | G5 13.3% |
| `grpc-dotnet` | 1024 | 5.88 | 0.167 | 0.322 | 0.405 | 273 | G5 18.9% |
| `grpc-dotnet` | 4096 | 5.18 | 0.189 | 0.339 | 0.422 | 273 |  |
| `zlink-dotnet` | 1024 | 7.52 | 0.131 | 0.179 | 0.250 | 276 |  |
| `zlink-dotnet` | 4096 | 6.83 | 0.143 | 0.185 | 0.260 | 272 |  |
| `zlink-framework-dotnet` | 1024 | 1.56 | 0.638 | 0.905 | 1.155 | 272 |  |
| `zlink-framework-dotnet` | 4096 | 1.55 | 0.639 | 0.909 | 1.309 | 271 |  |
| `grpc-java` | 1024 | 6.00 | 0.166 | 0.224 | 0.302 | 298 |  |
| `grpc-java` | 4096 | 6.00 | 0.167 | 0.219 | 0.302 | 299 |  |
| `zlink-java` | 1024 | 6.61 | 0.151 | 0.202 | 0.283 | 304 |  |
| `zlink-java` | 4096 | 6.64 | 0.150 | 0.198 | 0.264 | 300 |  |
| `zlink-framework-java` | 1024 | 0.47 | 2.137 | 2.579 | 2.765 | 271 |  |
| `zlink-framework-java` | 4096 | 0.47 | 2.150 | 2.564 | 2.676 | 271 |  |
| `grpc-node` | 1024 | 3.91 | 0.253 | 0.432 | 0.621 | 277 |  |
| `grpc-node` | 4096 | 3.72 | 0.266 | 0.453 | 0.660 | 279 |  |
| `zlink-node` | 1024 | 9.45 | 0.105 | 0.186 | 0.302 | 290 |  |
| `zlink-node` | 4096 | 9.27 | 0.107 | 0.178 | 0.311 | 288 |  |
| `zlink-framework-node` | 1024 | unsupported | — | — | — | — | codec에 protobuf bytes 종류가 없음 |
| `zlink-framework-node` | 4096 | unsupported | — | — | — | — | codec에 protobuf bytes 종류가 없음 |
| `grpc-cpp` | 1024 | 13.69 | 0.071 | 0.102 | 0.143 | — |  |
| `grpc-cpp` | 4096 | 13.41 | 0.072 | 0.108 | 0.147 | — |  |
| `zlink-cpp` | 1024 | 7.67 | 0.130 | 0.165 | 0.238 | — |  |
| `zlink-cpp` | 4096 | 7.39 | 0.135 | 0.170 | 0.248 | — | G5 15.5% |
| `zlink-framework-cpp` | 1024 | 0.49 | 2.020 | 2.990 | 3.321 | — |  |
| `zlink-framework-cpp` | 4096 | 0.53 | 1.881 | 2.250 | 3.190 | — |  |

#### request-window

| 구현 | payload | 처리량 (KOPS) | 지연 mean (ms) | p95 (ms) | p99 (ms) | drain (ms) | 비고 |
|---|---:|---:|---:|---:|---:|---:|---|
| `grpc-c` | 1024 | 62.58 | 1.579 | 1.802 | 2.038 | — |  |
| `grpc-c` | 4096 | 55.88 | 1.763 | 2.026 | 2.253 | — |  |
| `zlink-c` | 1024 | 463.00 | 0.177 | 0.283 | 0.357 | — | G5 12.0% |
| `zlink-c` | 4096 | 382.50 | 0.207 | 0.347 | 0.432 | — | G5 11.8% |
| `grpc-dotnet` | 1024 | 139.57 | 0.712 | 2.652 | 3.633 | 271 |  |
| `grpc-dotnet` | 4096 | 99.38 | 0.999 | 2.740 | 3.833 | 271 |  |
| `zlink-dotnet` | 1024 | 98.90 | 1.009 | 1.764 | 2.810 | 273 |  |
| `zlink-dotnet` | 4096 | 89.91 | 1.107 | 2.095 | 3.024 | 275 |  |
| `zlink-framework-dotnet` | 1024 | 12.28 | 7.882 | 9.212 | 15.535 | 274 |  |
| `zlink-framework-dotnet` | 4096 | 11.45 | 8.684 | 10.106 | 104.741 | 275 |  |
| `grpc-java` | 1024 | 111.44 | 0.915 | 1.131 | 1.305 | 452 |  |
| `grpc-java` | 4096 | 91.38 | 1.105 | 1.577 | 2.047 | 486 |  |
| `zlink-java` | 1024 | 0 | 0.000 | 0.000 | 0.000 | 272 | FB-050 |
| `zlink-java` | 4096 | 0 | 0.000 | 0.000 | 0.000 | 273 | FB-050 |
| `zlink-framework-java` | 1024 | 2.34 | 1.973 | 2.844 | 3.102 | 275 |  |
| `zlink-framework-java` | 4096 | 2.28 | 2.013 | 2.899 | 3.199 | 276 |  |
| `grpc-kotlin` | 1024 | 87.30 | 1.062 | 1.418 | 2.248 | 472 |  |
| `zlink-framework-kotlin` | 1024 | 2.26 | 2.013 | 2.892 | 3.142 | 275 |  |
| `grpc-node` | 1024 | 13.53 | 7.394 | 12.117 | 16.030 | 291 |  |
| `grpc-node` | 4096 | 9.80 | 10.214 | 16.305 | 20.703 | 285 |  |
| `zlink-node` | 1024 | 0.07 | 6,452.798 | 30,001.253 | 30,001.344 | 268 | FB-049, G5 47.7% |
| `zlink-node` | 4096 | 0.03 | 12,988.932 | 30,001.336 | 30,001.905 | 270 | FB-049, G5 769.9% |
| `zlink-framework-node` | 1024 | unsupported | — | — | — | — | codec에 protobuf bytes 종류가 없음 |
| `zlink-framework-node` | 4096 | unsupported | — | — | — | — | codec에 protobuf bytes 종류가 없음 |
| `grpc-cpp` | 1024 | 58.88 | 1.697 | 1.939 | 2.128 | — |  |
| `grpc-cpp` | 4096 | 55.98 | 1.784 | 2.048 | 2.293 | — |  |
| `zlink-cpp` | 1024 | 176.31 | 0.567 | 0.665 | 0.777 | — |  |
| `zlink-cpp` | 4096 | 162.10 | 0.616 | 0.729 | 0.842 | — |  |
| `zlink-framework-cpp` | 1024 | 1.06 | 92.962 | 105.566 | 110.318 | — |  |
| `zlink-framework-cpp` | 4096 | 1.06 | 92.724 | 111.463 | 121.507 | — |  |

#### request-backpressure

| 구현 | payload | 처리량 (KOPS) | 지연 mean (ms) | p95 (ms) | p99 (ms) | drain (ms) | 비고 |
|---|---:|---:|---:|---:|---:|---:|---|
| `grpc-c` | 1024 | 29.80 | 2,497.207 | 3,412.125 | 3,439.610 | — |  |
| `grpc-c` | 4096 | 31.15 | 2,182.147 | 3,897.705 | 3,968.772 | — | G5 13.7% |
| `zlink-c` | 1024 | 554.47 | 0.882 | 1.599 | 1.968 | — |  |
| `zlink-c` | 4096 | 428.11 | 0.358 | 0.634 | 0.826 | — |  |
| `grpc-dotnet` | 1024 | 70.41 | 21.422 | 29.807 | 50.364 | 274 |  |
| `grpc-dotnet` | 4096 | 55.06 | 15.679 | 44.859 | 57.773 | 271 |  |
| `zlink-dotnet` | 1024 | 32.44 | 0.226 | 0.360 | 0.511 | 273 |  |
| `zlink-dotnet` | 4096 | 21.61 | 0.250 | 0.393 | 0.591 | 273 |  |
| `zlink-framework-dotnet` | 1024 | 0.60 | 12,710.811 | 43,646.096 | 43,662.458 | 274 | FB-047, G5 254.7% |
| `zlink-framework-dotnet` | 4096 | 2.94 | 405.388 | 107.218 | 10,398.138 | 273 | FB-047, G5 89.8% |
| `grpc-java` | 1024 | 244.26 | 18,675.537 | 18,806.847 | 18,819.763 | 338 | G5 10.1% |
| `grpc-java` | 4096 | 129.42 | 2,187.882 | 3,707.333 | 4,041.575 | 350 |  |
| `zlink-java` | 1024 | 0 | 22,044.028 | 22,044.028 | 22,044.028 | 278 | FB-050 |
| `zlink-java` | 4096 | 0 | 29,420.284 | 29,420.284 | 29,420.284 | 275 | FB-050 |
| `zlink-framework-java` | 1024 | 2.27 | 1.985 | 2.855 | 3.113 | 275 |  |
| `zlink-framework-java` | 4096 | 2.32 | 1.985 | 2.850 | 3.098 | 277 |  |
| `grpc-node` | 1024 | 19.45 | 4,171.189 | 4,662.692 | 4,692.328 | 291 |  |
| `grpc-node` | 4096 | 15.27 | 5,291.201 | 5,653.218 | 5,672.899 | 287 |  |
| `zlink-node` | 1024 | 2.93 | 29,507.946 | 30,351.553 | 30,358.913 | 278 | FB-049 |
| `zlink-node` | 4096 | 0.08 | 27,772.410 | 33,796.750 | 33,797.073 | 269 | FB-049, G5 65.1% |
| `zlink-framework-node` | 1024 | unsupported | — | — | — | — | codec에 protobuf bytes 종류가 없음 |
| `zlink-framework-node` | 4096 | unsupported | — | — | — | — | codec에 protobuf bytes 종류가 없음 |
| `grpc-cpp` | 1024 | 40.50 | 359.091 | 423.440 | 432.093 | — |  |
| `grpc-cpp` | 4096 | 38.36 | 254.929 | 329.454 | 421.988 | — |  |
| `zlink-cpp` | 1024 | 144.03 | 0.122 | 0.185 | 0.293 | — |  |
| `zlink-cpp` | 4096 | 133.27 | 0.125 | 0.197 | 0.284 | — |  |
| `zlink-framework-cpp` | 1024 | 1.06 | 1,584.653 | 2,916.859 | 3,030.535 | — |  |
| `zlink-framework-cpp` | 4096 | 1.16 | 690.904 | 837.247 | 863.881 | — |  |

#### send-saturation

| 구현 | payload | 처리량 (KMSG/s) | 지연 mean (ms) | p95 (ms) | p99 (ms) | drain (ms) | 비고 |
|---|---:|---:|---:|---:|---:|---:|---|
| `grpc-c` | 1024 | 57.55 | — | — | — | — |  |
| `grpc-c` | 4096 | 49.36 | — | — | — | — |  |
| `zlink-c` | 1024 | 689.19 | — | — | — | — |  |
| `zlink-c` | 4096 | 481.24 | — | — | — | — |  |
| `grpc-dotnet` | 1024 | 34.66 | 0.117 | 0.216 | 0.266 | 326 |  |
| `grpc-dotnet` | 4096 | 32.83 | 0.126 | 0.218 | 0.269 | 327 |  |
| `zlink-dotnet` | 1024 | 705.89 | 275.974 | 341.069 | 376.532 | 1027 |  |
| `zlink-dotnet` | 4096 | 377.06 | 141.821 | 185.627 | 226.306 | 601 |  |
| `zlink-framework-dotnet` | 1024 | 106.58 | 1,692.417 | 2,857.424 | 2,992.571 | 9725 |  |
| `zlink-framework-dotnet` | 4096 | 84.80 | 1,860.129 | 3,239.903 | 3,346.605 | 9630 | G5 46.1% |
| `grpc-java` | 1024 | 40.84 | 0.106 | 0.156 | 0.191 | 375 |  |
| `grpc-java` | 4096 | 41.46 | 0.104 | 0.151 | 0.180 | 398 |  |
| `zlink-java` | 1024 | 544.59 | 0.074 | 0.202 | 0.264 | 429 |  |
| `zlink-java` | 4096 | 348.09 | 0.074 | 0.221 | 0.272 | 446 |  |
| `zlink-framework-java` | 1024 | 10.52 | 2,433.846 | 2,593.367 | 2,621.482 | 2318 | G5 16.5% |
| `zlink-framework-java` | 4096 | 9.87 | 832.710 | 905.903 | 927.347 | 920 | G5 14.4% |
| `grpc-node` | 1024 | 9.89 | 0.364 | 0.607 | 0.977 | 289 |  |
| `grpc-node` | 4096 | 9.53 | 0.381 | 0.654 | 1.076 | 288 |  |
| `zlink-node` | 1024 | 265.84 | 261.539 | 529.316 | 550.065 | 1204 |  |
| `zlink-node` | 4096 | 139.93 | 0.284 | 1.284 | 3.123 | 493 |  |
| `zlink-framework-node` | 1024 | unsupported | — | — | — | — | codec에 protobuf bytes 종류가 없음 |
| `zlink-framework-node` | 4096 | unsupported | — | — | — | — | codec에 protobuf bytes 종류가 없음 |
| `grpc-cpp` | 1024 | 41.16 | 0.123 | 0.183 | 0.226 | — |  |
| `grpc-cpp` | 4096 | 39.41 | 0.129 | 0.191 | 0.225 | — |  |
| `zlink-cpp` | 1024 | 380.51 | 0.057 | 0.103 | 0.187 | — |  |
| `zlink-cpp` | 4096 | 336.75 | 0.068 | 0.130 | 0.259 | — |  |
| `zlink-framework-cpp` | 1024 | 0 | 0.000 | 0.000 | 0.000 | — | FB-054 |
| `zlink-framework-cpp` | 4096 | 0 | 0.000 | 0.000 | 0.000 | — | FB-054 |

### 같은 언어 안의 배율

같은 언어의 두 구현을 나눈 값이다(중앙값 기준). 언어 사이를 가로질러 읽지 않는다(§4).

| 언어 | 패턴 | payload | zlink raw / gRPC | framework / raw |
|---|---|---:|---:|---:|
| c | request-serial | 1024 | 0.59 | — |
| c | request-serial | 4096 | 0.62 | — |
| c | request-window | 1024 | 7.40 | — |
| c | request-window | 4096 | 6.85 | — |
| c | request-backpressure | 1024 | 18.61 | — |
| c | request-backpressure | 4096 | 13.74 | — |
| c | send-saturation | 1024 | 11.98 | — |
| c | send-saturation | 4096 | 9.75 | — |
| dotnet | request-serial | 1024 | 1.28 | 0.21 |
| dotnet | request-serial | 4096 | 1.32 | 0.23 |
| dotnet | request-window | 1024 | 0.71 | 0.12 |
| dotnet | request-window | 4096 | 0.90 | 0.13 |
| dotnet | request-backpressure | 1024 | 0.46 | 0.019 (FB-047) |
| dotnet | request-backpressure | 4096 | 0.39 | 0.14 (FB-047) |
| dotnet | send-saturation | 1024 | 20.37 | 0.15 |
| dotnet | send-saturation | 4096 | 11.49 | 0.22 |
| java | request-serial | 1024 | 1.10 | 0.071 |
| java | request-serial | 4096 | 1.11 | 0.070 |
| java | request-window | 1024 | 0.000 (FB-050) | — |
| java | request-window | 4096 | 0.000 (FB-050) | — |
| java | request-backpressure | 1024 | 0.000 (FB-050) | — |
| java | request-backpressure | 4096 | 0.000 (FB-050) | — |
| java | send-saturation | 1024 | 13.34 | 0.019 |
| java | send-saturation | 4096 | 8.40 | 0.028 |
| node | request-serial | 1024 | 2.42 | unsupported |
| node | request-serial | 4096 | 2.49 | unsupported |
| node | request-window | 1024 | 0.006 (FB-049) | unsupported |
| node | request-window | 4096 | 0.003 (FB-049) | unsupported |
| node | request-backpressure | 1024 | 0.15 (FB-049) | unsupported |
| node | request-backpressure | 4096 | 0.005 (FB-049) | unsupported |
| node | send-saturation | 1024 | 26.89 | unsupported |
| node | send-saturation | 4096 | 14.69 | unsupported |
| cpp | request-serial | 1024 | 0.56 | 0.065 |
| cpp | request-serial | 4096 | 0.55 | 0.072 |
| cpp | request-window | 1024 | 2.99 | 0.006 |
| cpp | request-window | 4096 | 2.90 | 0.007 |
| cpp | request-backpressure | 1024 | 3.56 | 0.007 |
| cpp | request-backpressure | 4096 | 3.47 | 0.009 |
| cpp | send-saturation | 1024 | 9.24 | 0.000 (FB-054) |
| cpp | send-saturation | 4096 | 8.55 | 0.000 (FB-054) |

## 3. 언어별 동반 정보

규격 §7.1이 요구하는 동반 정보다. 상세 값은 언어별 문서 §3·§4에 있다.

| 언어 | warmup | gRPC server 구성 | source 포화 계측기 (상한) | 특이 사항 |
|---|---|---|---|---|
| C | 기본 | grpc++ synchronous | 없음 | 기준선. send 지연 없음 |
| .NET | 1,000회 | Kestrel HTTP/2 `AddGrpc()` 기본 | `submit_thread_cores` (1 / send 8) | framework send drain 9.6~9.7 s; backpressure 오류 2,511건(FB-047) |
| Java | 20 s | grpc-netty-shaded 기본 | `jvm_thread_cores` (1 / send 8) | grpc-java backpressure는 제출을 무제한으로 받아 in-flight 1M+ (도달 깊이) |
| Kotlin(보조) | 20 s | Java B 재사용 | 같음 | `request-window @1024`만 |
| Node.js | 1,000회 | `@grpc/grpc-js` `Server` 기본 | event loop 사용률 | framework 행 unsupported(codec bytes) |
| C++ | 5 s | `ServerBuilder` synchronous 기본 | `submit_thread_cores` (1 / send 8) | framework send는 warmup flood 뒤 전부 실패(FB-054) |

## 4. 읽는 방법

- **언어 사이 비교는 하지 않는다**(규격 §7.3). 런타임·gRPC 구현·warmup·GC가 다르다. 비교는 같은 언어의
  세 행 사이에서만 한다. C 기준선은 Core가 낼 수 있는 값을 보여 줄 뿐 다른 언어의 목표가 아니다.
- `request-serial`은 왕복 지연 승부다. 모든 언어에서 gRPC가 ZLink raw보다 짧은 지연(약 0.07 vs 0.12 ms)을
  보이며, 이는 Core의 연결별 I/O 스레드 배치(FB-048) 때문이다.
- `request-window`·`request-backpressure`·`send-saturation`은 동시성 승부다. C·C++·Node raw는 gRPC보다
  수 배에서 십수 배 높고, .NET raw는 send에서 20배 앞서지만 window에서는 gRPC보다 낮다(0.71).
- Framework 행은 raw의 5~24%다(.NET 0.12~0.24, Java 0.05~0.07, C++ 0.006~0.07). 언어별 구현 문제가
  아니라 공통 구조 비용으로 보이며 1.0 성능 항목으로 넘겼다(FB-047·FB-052).
- `request-backpressure`는 application 상한이 없으므로 "도달 깊이"가 결과다. grpc-java는 100만 건 이상,
  grpc-c는 수 초 지연, ZLink raw는 ms 단위로 각각 다른 지점에서 멈춘다.
- `unsupported`는 그 언어에서 그 행을 측정할 수 없었다는 뜻이고(Node framework의 codec bytes), `FB-nnn`은
  측정은 했지만 결함으로 값이 0이거나 오류가 섞여 판정에서 뺀 행이다.

## 5. 한계

- 단일 머신(WSL2, 8 vCPU) loopback이다. 네트워크·다중 노드·TLS는 범위 밖이다.
- C 기준선의 request 행은 G5 10~13%로 재현성 상한을 넘겨 formula 1(raw/C)의 분모로 쓸 수 없었다(FB-048).
- Node·Java raw의 request-window는 binding 0.17.6 결함(FB-049·FB-050)으로 completion/reply가 유실돼
  값이 0에 가깝다. 수정 뒤 재측정한다.
- C++ framework send-saturation은 warmup flood 뒤 RouteMesh send target이 사라져(FB-054) 값이 0이다.
- send 행의 `KMSG/s`는 규격대로 active-header 수신 수 / 5 s이며, drain이 긴 행(.NET framework 9.7 s,
  Node raw 1.2 s, Java framework 2.3 s)은 실제 소비율이 표보다 낮다.

## 6. 원본 위치

- 집계 기록: `doc/plan/fw-bench-worklog/results/{s1-dotnet-c,s2-node,s2-java-kotlin,s3-cpp}-2026-09-09.md`
  (집계기 `--format full` 출력 그대로: 표·RESULT 라인·중앙값·진단·G5·판정).
- 셀 원본(JSON·로그): `framework/bench/grpc/log/<lang>/…`(gitignore, 측정 머신 보존).
- 결정 기록: `doc/plan/fw-bench-worklog/decisions.ko.md` FB-045~FB-055.

## 부록 — formula 판정 (규격 §7.2, `request-window`)

formula 1 = `zlink-<lang> / zlink-c`, formula 2 = `zlink-framework-<lang> / zlink-<lang>`, 합격선 0.80.

| 언어 | formula 1 @1024 / @4096 | formula 2 @1024 / @4096 | 상태 |
|---|---|---|---|
| .NET | (0.214) / (0.235) unsupported — 분모 zlink-c G5 12.0% / 11.8% | **0.124 / 0.127 fail** | incomplete |
| Java | unsupported — 분자 zlink-java 유실(FB-050), 분모 G5 | unsupported — 분모 유실 | incomplete |
| Node.js | unsupported — 분자 G5 47.7% / 769.9%(FB-049), 분모 G5 | unsupported — framework 미측정 | incomplete |
| C++ | (0.381) / (0.424) unsupported — 분모 zlink-c G5 | **0.006 / 0.007 fail** | incomplete |
