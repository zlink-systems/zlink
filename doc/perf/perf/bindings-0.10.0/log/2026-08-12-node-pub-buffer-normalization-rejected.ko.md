# Node PUB/SUB scalar Buffer normalization 후보 결과

## 대상과 조건

`MULTI_PUBSUB / tcp`만 C를 먼저, Node를 다음에 단독 실행했다. Release Core
`0.10.1`, clients `100`, duration `1초`, runs `1`, balanced auto-HWM,
message size `64, 256, 1024, 4096, 65536, 131072B`를 사용했다.

## 결과

| 구현 | C 대비 throughput 비율 (size 순서) | 산술평균 | 판정 |
|------|-----------------------------------|---------:|------|
| scalar Buffer가 generic MessageLike 정규화를 생략 | 20.30 / 22.81 / 25.82 / 34.09 / 39.61 / 46.30% | 31.49% | 원복 |

이 측정 시점의 active receive loop는 C와 종료 deadline을 동일하게 적용하지 않았으므로,
다른 결과와 수치를 직접 비교하지 않는다. 후보가 현 수신 계약을 유지한 채 확실한 개선을
제시하지 못했으므로 generic normalization은 유지한다. public interface, Message ownership,
multipart와 backpressure 계약은 변경하지 않았다.

## 결과 파일

- C: `/tmp/zlink-node-pub-normalize-c/multi/report/perf_c_multi_linux_20260812_215855.txt`
- Node: `/tmp/zlink-node-pub-normalize-node/multi/report/perf_node_multi_linux_20260812_215929.txt`
