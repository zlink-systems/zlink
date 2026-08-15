# Java PUBSUB direct outbound frame 측정 결과

## 대상

`MULTI_PUBSUB / tcp` publisher가 template `Message`를 만들고 outbound `Message`로 다시
복사하던 경로를, outbound native `Message` 하나를 직접 생성해 보내는 후보로 바꿔 측정했다.
public interface와 Core ABI는 변경하지 않았다.

Release Core `0.10.1`, clients `100`, duration `1초`, runs `1`, balanced auto-HWM에서
C를 먼저, Java를 다음에 단독 실행했다.

## 결과

| Message size | C throughput | Java throughput | C 대비 비율 |
|---|---:|---:|---:|
| 64B | 1,499,410 msg/s | 532,262 msg/s | 35.50% |
| 256B | 1,471,491 msg/s | 473,619 msg/s | 32.18% |
| 1024B | 1,279,698 msg/s | 181,839 msg/s | 14.21% |
| 4096B | 494,196 msg/s | 74,979 msg/s | 15.17% |
| 65536B | 98,205 msg/s | 44,387 msg/s | 45.20% |
| 131072B | 54,262 msg/s | 13,858 msg/s | 25.54% |
| 산술평균 | - | - | 27.97% |

최신 기준 `51.44%`보다 낮아 direct outbound 후보는 채택하지 않았다. Java FFM/native
ownership 경로에서는 source→outbound copy가 더 높은 throughput을 보였다.

- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_154052_java-pubsub-direct-outbound-c.txt`
- Java report: `/home/hep7hep7/project/zlink/bindings/java/perf/results/multi/report/perf_java_multi_linux_20260812_154113_java-pubsub-direct-outbound-java.txt`
