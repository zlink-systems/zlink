# Java PUBSUB caller-provided TopicMessage 측정 결과

## 대상과 조건

`MULTI_PUBSUB / tcp`만 C를 먼저, Java를 다음에 단독 실행했다. Release Core
`0.10.1`, clients `100`, duration `1초`, runs `1`, balanced auto-HWM,
message size `64, 256, 1024, 4096, 65536, 131072B`를 사용했다.

## 결과

| 구현 | C 대비 throughput 비율 (size 순서) | 산술평균 | 판정 |
|------|-----------------------------------|---------:|------|
| SUB별 caller-provided `TopicMessage` 재사용 | 42.98 / 42.67 / 32.52 / 23.58 / 47.70 / 64.69% | 42.36% | 원복 |

`TopicMessage`가 이전 part를 정리하고 단일 part state를 갱신하는 비용이 기존 wrapper pool 경로보다
커져 성능이 하락했다. public interface와 ownership은 변경하지 않았고 후보 구현은 원복했다.

## 결과 파일

- C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_132512_java-pub-received-reuse-c.txt`
- Java: `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260812_132532_java-pub-received-reuse.txt`
