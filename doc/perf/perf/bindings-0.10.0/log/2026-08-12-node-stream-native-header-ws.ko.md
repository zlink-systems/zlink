# Node STREAM WS native header 재측정 결과

## 대상과 조건

`MULTI_STREAM / ws`만 C를 먼저, Node를 다음에 단독 실행했다. Release Core `0.10.1`,
clients `100`, duration `1초`, runs `1`, balanced auto-HWM, message size
`64, 256, 1024, 65536B`를 사용했다.

## 결과

| C 대비 throughput 비율 (size 순서) | 산술평균 | 최소 기준 | 판정 |
|-----------------------------------|---------:|----------:|------|
| 51.97 / 50.75 / 51.03 / 95.08% | 62.21% | 35% | 통과 |

native header materialization이 WS STREAM에도 적용된 상태를 재측정했다. public packet handler와
Message ownership은 변경하지 않았다.

## 결과 파일

- C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_115501_node-stream-native-header-ws-c.txt`
- Node: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_115515_node-stream-native-header-ws.txt`
