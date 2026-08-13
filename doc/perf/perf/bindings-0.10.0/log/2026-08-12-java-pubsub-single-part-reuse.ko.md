# Java PUBSUB single-part wrapper 재사용 측정 결과

## 대상

`MULTI_PUBSUB / tcp`의 caller-provided `TopicMessage`에 이전 single-part wrapper를 다시
arm하는 후보를 측정했다. public interface와 Core ABI는 변경하지 않았다.

Release Core `0.10.1`, clients `100`, duration `1초`, runs `1`, balanced auto-HWM에서
C를 먼저, Java를 다음에 단독 실행했다.

## 결과

| Message size | C throughput | Java throughput | C 대비 비율 |
|---|---:|---:|---:|
| 64B | 1,312,514 msg/s | 224,670 msg/s | 17.12% |
| 256B | 1,355,907 msg/s | 222,687 msg/s | 16.42% |
| 1024B | 1,161,789 msg/s | 84,309 msg/s | 7.26% |
| 4096B | 469,770 msg/s | 91,791 msg/s | 19.54% |
| 65536B | 83,912 msg/s | 37,015 msg/s | 44.11% |
| 131072B | 46,399 msg/s | 26,855 msg/s | 57.88% |
| 산술평균 | - | - | 27.06% |

최신 기준 `51.44%`보다 낮아 후보는 채택하지 않았다. fixed target을 re-arm할 때의 native
close/init 비용이 기존 wrapper pool의 acquire/owner-close 흐름보다 컸다.

- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_154316_java-pubsub-single-part-reuse-c.txt`
- Java report: `/home/hep7hep7/project/zlink/bindings/java/perf/results/multi/report/perf_java_multi_linux_20260812_154340_java-pubsub-single-part-reuse-java.txt`
