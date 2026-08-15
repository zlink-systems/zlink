# Java PUBSUB header allocation 후보 측정 결과

## 대상

Java `MULTI_PUBSUB` client가 payload header를 검사할 때 매 delivery마다
`PerfUtil.Header`를 만드는 경로를 allocation 없는 검사로 바꾼 후보를 비교했다.

Release Core `0.10.1`, TCP, clients `100`, duration `1초`, runs `1`, balanced
auto-HWM에서 C를 먼저, Java를 다음에 단독 실행했다.

## 결과

| Message size | C throughput | Java throughput | C 대비 비율 |
|---|---:|---:|---:|
| 64B | 1,642,223 msg/s | 667,913 msg/s | 40.67% |
| 256B | 1,515,760 msg/s | 648,205 msg/s | 42.76% |
| 1024B | 1,346,672 msg/s | 601,221 msg/s | 44.64% |
| 4096B | 514,420 msg/s | 267,032 msg/s | 51.91% |
| 65536B | 103,183 msg/s | 68,825 msg/s | 66.70% |
| 131072B | 55,399 msg/s | 35,196 msg/s | 63.53% |
| 산술평균 | - | - | 51.70% |

최소 기준 70%에 도달하지 못했다. `PerfUtil.Header`는 JIT가 escape analysis로 대부분
scalar replacement할 수 있고, 후보는 wire-header 검사를 harness에 중복하므로 채택하지 않았다.

- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_155239_java-pubsub-header-stack-c.txt`
- Java report: `/home/hep7hep7/project/zlink/bindings/java/perf/results/multi/report/perf_java_multi_linux_20260812_155301_java-pubsub-header-stack-java.txt`
