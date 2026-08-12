# Java PUBSUB topic cache 측정 결과

## 대상과 조건

`MULTI_PUBSUB / tcp`만 C를 먼저, Java를 다음에 단독 실행했다. Release Core
`0.10.1`, clients `100`, duration `1초`, runs `1`, balanced auto-HWM,
message size `64, 256, 1024, 4096, 65536, 131072B`를 사용했다.

## 결과

| 구현 | C 대비 throughput 비율 (size 순서) | 산술평균 | 판정 |
|------|-----------------------------------|---------:|------|
| 최근 topic 문자열 cache | 49.97 / 44.58 / 33.13 / 42.96 / 62.52 / 62.82% | 49.33% | 원복 |

최근 topic의 bytes와 길이를 비교해 UTF-8 `String` 재생성을 피하는 후보를 적용했다. 비교와
cache 갱신 비용이 이 조건의 이득보다 커서 Java simple one-way 최소 기준 `70%`에 도달하지
못했다. public interface와 message ownership은 변경하지 않았고 후보 구현은 원복했다.

## 결과 파일

- C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_122829_java-topic-cache-c.txt`
- Java: `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260812_122850_java-topic-cache.txt`
