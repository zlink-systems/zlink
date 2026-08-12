# Java Multi PUBSUB 및 전체 평균 결과

## 측정 조건

- Core: `0.10.1` release runtime
- 순서: C를 먼저 실행하고 Java를 다음에 실행
- 병렬 실행: 없음
- 대상: `MULTI_PUBSUB / tcp`
- size: 64·256·1024·4096·65536·131072B
- clients: 100, duration: 1초, runs: 1, balanced auto-HWM

## 측정값

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 1,495,407 msg/s | 656,552 msg/s | 43.905% |
| 256B | 1,641,211 msg/s | 762,186 msg/s | 46.440% |
| 1024B | 1,222,996 msg/s | 552,171 msg/s | 45.149% |
| 4096B | 544,276 msg/s | 203,470 msg/s | 37.384% |
| 65536B | 106,559 msg/s | 69,000 msg/s | 64.753% |
| 131072B | 52,014 msg/s | 41,631 msg/s | 80.038% |

throughput ratio 산술평균은 52.945%다. 이전 최종 행의 46.808%보다 6.137%p 높다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_094339_java-multi-pubsub-topic-invoker-c.txt`
- Java report: `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260812_094352_java-multi-pubsub-topic-invoker.txt`

## 전체 평균

9.3절 Java 최종 paired 표본은 412개 size cell이다. 최신 TCP `PAIR`, `DEALER_DEALER`,
`PUBSUB`, `MULTI_DEALER_DEALER`, `MULTI_DEALER_ROUTER`, `MULTI_PUBSUB` 결과로 같은
행을 교체한 산술평균은 80.318%다. 9.2절 .NET 최종 paired 표본 418개 size cell의 산술평균은
82.627%다.

Node의 과거 batch 기반 표본은 batch 미사용 현재 구현과 같은 결과 집합이 아니므로 전체 평균에
사용하지 않는다.
