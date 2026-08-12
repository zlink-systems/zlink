# Node PUBSUB caller-provided storage 해제 순서 후보 결과

## 대상과 조건

`MULTI_PUBSUB / tcp`만 C를 먼저, Node를 다음에 단독 실행했다. Release Core `0.10.1`,
clients `100`, duration `1초`, runs `1`, balanced auto-HWM, message size
`64, 256, 1024, 4096, 65536, 131072B`를 사용했다.

## 결과

| 구현 | C 대비 throughput 비율 (size 순서) | 산술평균 | 판정 |
|------|-----------------------------------|---------:|------|
| 표준 caller-provided `TopicMessage` 수신 | 20.98 / 22.75 / 21.66 / 29.10 / 37.95 / 50.93% | 30.56% | 비교값 |
| 이전 part를 먼저 해제한 뒤 새 part materialize | 22.89 / 21.33 / 22.06 / 24.81 / 46.75 / 47.49% | 30.89% | 원복 |

이전 part를 먼저 해제해 wrapper pool을 같은 호출에서 재사용하는 후보는 public `TopicMessage`와
`Message` 소유권을 유지했다. 그러나 TCP 최소 평균 35%에 도달하지 못해 채택하지 않았다.

## 결과 파일

- C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_115546_node-pub-final-baseline-c.txt`
- Node 표준 비교: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_115618_node-pub-final-baseline.txt`
- Node 후보: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_115732_node-pub-release-before-materialize.txt`
