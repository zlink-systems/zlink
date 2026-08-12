# Java multi harness C parity 정렬 결과

## 변경 범위

Java multi runner에서 C와 달랐던 latency reservoir, DEALER/DEALER deadline,
routed relay 종료, PUB/SUB stop token, STREAM backpressure·timeout을 C 기준으로
정렬했다. 공개 binding interface는 변경하지 않았다.

STREAM은 private pending queue로 `DONT_WAIT` send의 transient backpressure를
보존하고 send-ready callback에서 같은 routing id와 native `Message`를 순서대로 재전송한다.
queue는 packet callback과 send-ready callback 사이에서 lock으로 보호한다.

## 개별 재측정 결과

Release Core `0.10.1`, `tcp`, clients `100`, duration `1초`, runs `1`, balanced
auto-HWM에서 C를 먼저, Java를 다음에 단독 실행했다.

| Pattern | C 대비 throughput 비율 (size 순서) | 산술평균 | 판정 기준 |
|---|---|---:|---|
| `MULTI_DEALER_ROUTER_SENDSEND` | 51.19 / 51.17 / 52.36 / 55.15 / 117.45 / 104.06% | 71.90% | multi routed echo 목표 70% 통과 |
| `MULTI_DEALER_DEALER` | 52.85 / 68.37 / 93.33 / 66.47 / 51.17 / 75.65% | 67.97% | 단순 one-way 목표 90% 미달 |
| `MULTI_PUBSUB` | 39.08 / 44.13 / 48.73 / 49.57 / 58.33 / 71.98% | 52.64% | 단순 one-way 목표 90% 미달 |
| `MULTI_STREAM` | 73.87 / 72.57 / 66.41 / 92.46% | 76.33% | STREAM 재측정값 |

## 결과 파일

- DEALER/ROUTER C: `/tmp/zlink-java-relay-parity-c/multi/report/perf_c_multi_linux_20260812_222232.txt`
- DEALER/ROUTER Java: `/tmp/zlink-java-relay-parity-java/multi/report/perf_java_multi_linux_20260812_222251.txt`
- DEALER/DEALER C: `/tmp/zlink-java-dd-parity-c/multi/report/perf_c_multi_linux_20260812_222507.txt`
- DEALER/DEALER Java: `/tmp/zlink-java-dd-parity-java/multi/report/perf_java_multi_linux_20260812_222529.txt`
- PUB/SUB C: `/tmp/zlink-java-pub-parity-c/multi/report/perf_c_multi_linux_20260812_222635.txt`
- PUB/SUB Java: `/tmp/zlink-java-pub-parity-java/multi/report/perf_java_multi_linux_20260812_222655.txt`
- STREAM C: `/tmp/zlink-java-stream-parity-c/multi/report/perf_c_multi_linux_20260812_222433.txt`
- STREAM Java: `/tmp/zlink-java-stream-parity-java/multi/report/perf_java_multi_linux_20260812_222451.txt`
