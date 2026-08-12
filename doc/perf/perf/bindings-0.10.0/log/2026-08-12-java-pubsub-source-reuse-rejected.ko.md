# Java MULTI_PUBSUB source 재사용 결과

## 대상과 조건

- 대상: `tcp`, `ws`, `wss`, `tls`의 `MULTI_PUBSUB`
- 조건: transport를 한 번에 하나씩 순차 실행했다. 각 실행은 C를 먼저 단독 실행한 뒤 Java를 단독 실행했고, `duration=1`, `runs=1`, `clients=100`, balanced auto-HWM을 사용했다.
- Core: release `0.10.1`

## 변경

active window 동안 전송하지 않는 원본 `Message` 하나를 유지하고, 매 publish에서 header만 갱신하도록 변경했다. `Message.from(source)`가 전송용 frame을 새로 만들어 ownership과 backpressure 동작은 유지한다.

## 결과

| Transport | Java/C 산술 평균 | 기존 최종 값 | 판정 |
|-----------|-----------------:|-------------:|------|
| tcp | 31.71% | 53.55% | 하락 |
| ws | 39.05% | 45.40% | 하락 |
| wss | 43.52% | 39.61% | 상승, 기준 미달 |
| tls | 30.00% | 29.38% | 상승, 기준 미달 |

네 transport 중 두 곳이 크게 하락했고 어떤 transport도 70% 기준에 도달하지 못했다. 변경을 되돌렸다.

- C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_133233_java-pub-source-reuse-c.txt`
- Java: `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260812_133320_java-pub-source-reuse.txt`

