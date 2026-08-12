# Java PUBSUB topic invoker 측정 결과

## 대상과 조건

`MULTI_PUBSUB / tcp`만 C를 먼저, Java를 다음에 단독 실행했다. Release Core
`0.10.1`, clients `100`, duration `1초`, runs `1`, balanced auto-HWM,
message size `64, 256, 1024, 4096, 65536, 131072B`를 사용했다.

## 결과

| 구현 | C 대비 throughput 비율 (size 순서) | 산술평균 | 판정 |
|------|-----------------------------------|---------:|------|
| 반복 topic의 publish invoker 재사용 | 47.37 / 38.28 / 37.04 / 43.67 / 66.87 / 88.10% | 53.55% | 채택, 최소 기준 미달 |
| 동일 `String` topic의 UTF-8 검증 cache 추가 | 42.26 / 39.21 / 44.07 / 48.35 / 74.60 / 60.45% | 51.49% | 원복 |

`publish(topic)`은 항상 새 `SendOperation`을 반환하고 topic invoker는 mutable state가 없다.
따라서 socket별 최근 invoker 하나를 재사용해 public interface와 message ownership을 유지했다.
검증 cache는 이 조건에서 종합 성능을 낮춰 원복했다.

## 결과 파일

- invoker C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_123215_java-topic-invoker-cache-c.txt`
- invoker Java: `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260812_123238_java-topic-invoker-cache.txt`
- 검증 cache C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_123350_java-topic-validation-cache-c.txt`
- 검증 cache Java: `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260812_123411_java-topic-validation-cache.txt`
